#include <FreeRTOS.h>
#include <task.h>
#include <stdbool.h>
#include <stdio.h>
#include "piano_scanner.h"
#include "drivers/bcm2835.h"
#include "bcm2835_miniuart.h"

#define PS_KEY_STATE_IDLE 0
#define PS_KEY_STATE_STARTED 1
#define PS_KEY_STATE_HIT 2

typedef struct
{
    uint32_t press_time;
    uint32_t state;
} key_data_t;

key_data_t key_data[PS_NUMBER_OF_KEY_BANKS * PS_NUMBER_OF_KEYS_PER_BANK];

static char midi_out_buffer[PS_MIDI_OUT_BUFFER_SIZE_BYTES];
static int midi_out_buffer_in_index;
static int midi_out_buffer_out_index;
// Empty condition midi_out_buffer_in_index == midi_out_buffer_out_index
#define MIDI_OUT_BUFFER_EMPTY (midi_out_buffer_out_index == midi_out_buffer_in_index)
#define MIDI_OUT_BUFFER_FULL (midi_out_buffer_out_index == 0 ? midi_out_buffer_in_index == PS_MIDI_OUT_BUFFER_SIZE_BYTES - 1 : midi_out_buffer_in_index == midi_out_buffer_out_index - 1)
// Full condition midi_out_buffer_out_index == midi_out_buffer_in_index + 1  (modulo PS_MIDI_OUT_BUFFER_SIZE_BYTES)
#define MIDI_OUT_BUFFER_INDEX_INCREMENT(index) (index = (index < PS_MIDI_OUT_BUFFER_SIZE_BYTES - 1 ? index + 1 : 0))

void ps_producer_task(void *params);

// The consumer task is this function - run cooperatively
void ps_consume_char_from_buffer_if_possible()
{
    if (MIDI_OUT_BUFFER_EMPTY) return;
    if(UART_TX_READY())
    {
        UART_TX_CHAR(midi_out_buffer[midi_out_buffer_out_index]); // block if no fifo space available
        MIDI_OUT_BUFFER_INDEX_INCREMENT(midi_out_buffer_out_index);
    }
}

char ps_map_key_to_note(int key)
{
    return (char) (key + PS_MIDI_NOTE_KEY0_OFFSET);
}

char ps_map_time_to_velocity(uint32_t key_time_us)
{
    float velocity = (key_time_us * PS_VELOCITY_MAPPING_SLOPE + PS_VELOCITY_MAPPING_OFFSET);
    PS_SATURATE(MIDI_MAX_VELOCITY, MIDI_MIN_VELOCITY, velocity);
    return (char)(velocity + 0.5);
}

void ps_send_char_to_buffer_blocking_if_full(char data)
{
    if(MIDI_OUT_BUFFER_FULL)
    {
        ps_consume_char_from_buffer_if_possible();
    }
    midi_out_buffer[midi_out_buffer_in_index] = data;
    MIDI_OUT_BUFFER_INDEX_INCREMENT(midi_out_buffer_in_index);
}

void ps_send_note_on(int key, uint32_t key_time_us)
{
    ps_send_char_to_buffer_blocking_if_full(MIDI_STATUS_NOTE_ON(PS_MIDI_CHANNEL));
    ps_send_char_to_buffer_blocking_if_full(ps_map_key_to_note(key));
    ps_send_char_to_buffer_blocking_if_full(ps_map_time_to_velocity(key_time_us));
}

void ps_send_note_off(int key)
{
    ps_send_char_to_buffer_blocking_if_full(MIDI_STATUS_NOTE_OFF(PS_MIDI_CHANNEL));
    ps_send_char_to_buffer_blocking_if_full(ps_map_key_to_note(key));
    ps_send_char_to_buffer_blocking_if_full(0); // Not sending note off velocity for now.
}

void ps_init(void)
{
    PS_LOG_FMT("Init Piano Scanner %i", 4);
	printf("Slope : %f\n\r", PS_VELOCITY_MAPPING_SLOPE);
	printf("Offset : %f\n\r", PS_VELOCITY_MAPPING_OFFSET);
	printf("Slope : %i\n\r", (int)PS_VELOCITY_MAPPING_SLOPE);
	printf("Offset : %i\n\r", (int)PS_VELOCITY_MAPPING_OFFSET);
	printf("80000 : %i\n\r", ps_map_time_to_velocity(80000));
	printf("90000 : %i\n\r", ps_map_time_to_velocity(90000));
	printf("2900 : %i\n\r", ps_map_time_to_velocity(2900));
	printf("1000 : %i\n\r", ps_map_time_to_velocity(1000));

    // set up gpio
    bcm2835_gpio_fsel(PS_SHIFT_REG_RESET_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_INPUT_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_CLOCK_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_LATCH_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);

    bcm2835_gpio_clr(PS_SHIFT_REG_RESET_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_INPUT_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_LATCH_GPIO_NUMBER);

    for (size_t pin = PS_KEY_0_PORT_GPIO_NUMBER; pin <= PS_KEY_7_PORT_GPIO_NUMBER; pin++)
    {
        bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_INPT);
        bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_DOWN);
    }

    // run consumer task

    // run producer task 
    BaseType_t ret = xTaskCreate(ps_producer_task, "key_producer", 512, NULL, 2, NULL);
    PS_LOG_FMT("Created key producer task %li", ret);
}

// This task scans the keyboard by clocking a shift
// register to walk a bit past all the make/break (m/b) (aka start/finish or switch1/2)
// switches. On the Roland EP 50 they are all normally low switches with inline diodes
// back to the m/b lines. Either an m or b line for one bank at a time is energised
// and then the 8 keys in the bank can be read on the gpio inputs
//
// Each scan loop, the consumer task is cooperatively run
// This is simply attempting to send a char to the uart if available
//
// The following state machine is implemented
//
//                  Start button down          End button down
//                   /record start time         /calc velocity
//                                              /queue hit
//       ┌─────────────────────────────┐ ┌───────────────────────┐
//       │                             │ │                       │
//       │                             │ │                       │
//  ┌────┴─────┐ Start button up  ┌────▼─┴───┐               ┌───▼──────┐
//  │          │ and time greater │          │               │          │
//  │          │ than debounce    │          │               │          │
//  │   IDLE   ◄──────────────────┤  START   │               │   DOWN   │
//  │          │                  │          │               │          │
//  │          │                  │          │               │          │
//  └────▲─────┘                  └──────────┘               └───┬──────┘
//       │                                                       │
//       │                   Start button up                     │
//       └───────────────────────────────────────────────────────┘

void ps_producer_task(void *params)
{
    uint32_t loops = 0;
    // uint32_t start_time;
    bool led_on = false;
    PS_LOG_FMT("Starting! %i", 1);
    for (;;)
    {
        // Consumer task is implemented here cooperatively
        // TODO - If we only have one task why are we using an rtos!??
        ps_consume_char_from_buffer_if_possible();

        // bcm2835_delay(500);
        
        // PS_LOG_FMT("Loop %lu", loops);
        loops++;
        if(loops % 1000 == 0)
        {
            if (led_on)
            {
                RUN_LED_OFF();
                led_on = false;
            }
            else
            {
                RUN_LED_ON();
                led_on = true;
            }
            // PS_LOG_FMT("LED %s", led_on ? "On" : "Off");
        }

        // uint8_t bits = GPIO_READ_BANK();
        // PS_LOG_FMT("BANK BITS %2X", bits);

        // start_time = READ_U32BIT_US_TIME();

        // Reset shift register and clock a 1 to output 0
        GPIO__LOW(PS_SHIFT_REG_RESET_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_INPUT_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_RESET_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER); // Clock the bit into the shift regs
        GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_LATCH_GPIO_NUMBER); // Latch the data out
        GPIO__LOW(PS_SHIFT_REG_LATCH_GPIO_NUMBER);
        GPIO__LOW(PS_SHIFT_REG_INPUT_GPIO_NUMBER);

        uint8_t bank_bits;
        for (size_t bank = 0; bank < PS_NUMBER_OF_KEY_BANKS; bank++)
        {
            // read start buttons of bank
            bank_bits = GPIO_READ_BANK();
            // note that because  all arithmatic using the time is done modulo 2^32
            // there is no need to account for timer roll over
            // for example: assuming modulo 10 and the timer rolls over
            // say start_time = 8 and end_time = 1
            // end_time - start_time == 3
            // this is the same as 10-8 + 1
            uint32_t current_time = READ_U32BIT_US_TIME();
            // if any start buttons set
            for (size_t position = 0; position < PS_NUMBER_OF_KEYS_PER_BANK; position++)
            {
                int key = bank * PS_NUMBER_OF_KEYS_PER_BANK + position;
                // if the start key is down
                bool button_down = bank_bits & (1 << position);

                switch (key_data[key].state)
                {
                case PS_KEY_STATE_IDLE:
                    if (button_down)
                    {
                        key_data[key].press_time = READ_U32BIT_US_TIME();
                        key_data[key].state = PS_KEY_STATE_STARTED;
                        PS_LOG_FMT("START: key:%i bank:%i, bit:%i ", key, bank, position);
                    }
                    break;
                case PS_KEY_STATE_STARTED:
                    if (!button_down && current_time - key_data[key].press_time > PS_DEBOUNCE_TIME_US)
                    {
                        key_data[key].state = PS_KEY_STATE_IDLE;
                        PS_LOG_FMT("NO HIT: key:%i bank:%i, bit:%i", key, bank, position);
                    }
                    break;
                case PS_KEY_STATE_HIT:
                    if (!button_down)
                    {
                        key_data[key].state = PS_KEY_STATE_IDLE;
                        PS_LOG_FMT("IDLE: key:%i bank:%i, bit:%i", key, bank, position);
                        ps_send_note_off(key);
                    }
                    break;
                }
            }

            // clock shift register to the end keys
            GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER); // Clock the shift reg
            GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
            GPIO_HIGH(PS_SHIFT_REG_LATCH_GPIO_NUMBER); // Latch the data out
            GPIO__LOW(PS_SHIFT_REG_LATCH_GPIO_NUMBER);

            bcm2835_delayMicroseconds(10);

            // read end buttons of bank
            bank_bits = GPIO_READ_BANK();
            current_time = READ_U32BIT_US_TIME();
            if (bank_bits) // nothing to do if no end buttons down
            {
                for (size_t position = 0; position < PS_NUMBER_OF_KEYS_PER_BANK; position++)
                {
                    int key = bank * PS_NUMBER_OF_KEYS_PER_BANK + position;
                    // if the end key is down
                    bool button_down = bank_bits & (1 << position);

                    switch (key_data[key].state)
                    {
                    case PS_KEY_STATE_IDLE:
                        if (button_down)
                        {
                            // illegal state - something must be wrong - log error
                            PS_LOG_FMT("ERROR end detected before start: key:%i bank:%i, bit:%i", key, bank, position);
                        }
                        break;
                    case PS_KEY_STATE_STARTED:
                        if (button_down)
                        {
                            uint32_t duration = current_time - key_data[key].press_time;
                            key_data[key].state = PS_KEY_STATE_HIT;
                            PS_LOG_FMT("HIT key:%i bank:%i, bit:%i, duration:%lu", key, bank, position, duration);
                            ps_send_note_on(key, duration);

                        }
                        break;
                    case PS_KEY_STATE_HIT:
                        // do nothing as we wait for start button to go back to idle
                        break;
                    }
                }
            }

            // clock shift register to next bank
            GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER); // Clock the shift reg
            GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
            GPIO_HIGH(PS_SHIFT_REG_LATCH_GPIO_NUMBER); // Latch the data out
            GPIO__LOW(PS_SHIFT_REG_LATCH_GPIO_NUMBER);

            bcm2835_delayMicroseconds(10);
        }
        
        // uint32_t end_time = READ_U32BIT_US_TIME();
        // PS_LOG_FMT("Start %lu end %lu", start_time, end_time);
        // PS_LOG_FMT("Elapsed %lu", end_time - start_time);
    }
}