#include <FreeRTOS.h>
#include <task.h>
#include <stdbool.h>
#include <stdio.h>
#include "piano_scanner.h"
#include "drivers/bcm2835.h"

#define PS_KEY_STATE_IDLE 0
#define PS_KEY_STATE_STARTED 1
#define PS_KEY_STATE_HIT 2

typedef struct 
{
  uint32_t press_time;
  uint32_t state;
} key_data_t;

key_data_t key_data[PS_NUMBER_OF_KEY_BANKS * PS_NUMBER_OF_KEYS_PER_BANK];

void ps_producer_task(void *params);


void ps_init(void)
{
    PS_LOG_FMT("Init Piano Scanner %i", 1);

    // set up gpio
    bcm2835_gpio_fsel(PS_SHIFT_REG_RESET_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_INPUT_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_CLOCK_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(PS_SHIFT_REG_LATCH_GPIO_NUMBER, BCM2835_GPIO_FSEL_OUTP);

    bcm2835_gpio_clr(PS_SHIFT_REG_RESET_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_INPUT_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
    bcm2835_gpio_clr(PS_SHIFT_REG_LATCH_GPIO_NUMBER);

    for (size_t pin = PS_KEY_7_PORT_GPIO_NUMBER; pin <= PS_KEY_7_PORT_GPIO_NUMBER; pin++)
    {
        bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_INPT);
    }
    
    // run consumer task

    // run producer task 
    xTaskCreate(ps_producer_task, "key_producer", 1024, NULL, 5, NULL);
}

// This task scans the keyboard by clocking a shift 
// register to walk a bit past all the make/break (m/b) (aka start/finish or switch1/2)
// switches. On the Roland EP 50 they are all normally low switches with inline diodes 
// back to the m/b lines. Either an m or b line for one bank at a time is energised
// and then the 8 keys in the bank can be read on the gpio inputs
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
    bool led_on = false;
    PS_LOG_FMT("Starting! %i", 1);
    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        PS_LOG_FMT("Loop %lu", loops);
        loops++;
        // if(loops % 10000 == 0)
        // {
            if(led_on)
            {
                RUN_LED_OFF();
                led_on = false;
            }
            else
            {
                RUN_LED_ON();
                led_on = true;
            }
            PS_LOG_FMT("LED %s", led_on ? "On" : "Off");
        // }

        // Reset shift register and clock a 1 to output 0
        GPIO__LOW(PS_SHIFT_REG_RESET_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_INPUT_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_RESET_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
        GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
        GPIO_HIGH(PS_SHIFT_REG_LATCH_GPIO_NUMBER);
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
                int key = bank * position;
                // if the start key is down
                bool button_down = bank_bits & (1 << position);
                
                switch(key_data[key].state)
                {
                    case PS_KEY_STATE_IDLE:
                        if(button_down)
                        {
                            key_data[key].press_time = READ_U32BIT_US_TIME();
                            key_data[key].state = PS_KEY_STATE_STARTED;
                            PS_LOG_FMT("START: key:%i bank:%i, bit:%i ", key, bank, position);
                        }
                        break;
                    case PS_KEY_STATE_STARTED:
                        if(!button_down && current_time - key_data[key].press_time > PS_DEBOUNCE_TIME_US)
                        {
                            key_data[key].state = PS_KEY_STATE_IDLE;
                            PS_LOG_FMT("NO HIT: key:%i bank:%i, bit:%i", key, bank, position);
                        }
                        break;
                    case PS_KEY_STATE_HIT:
                        if(!button_down)
                        {
                            key_data[key].state = PS_KEY_STATE_IDLE;
                            PS_LOG_FMT("IDLE: key:%i bank:%i, bit:%i", key, bank, position);
                        }
                        break;
                }
            }

            // clock shift register to the end keys
            GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
            GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);

            // read end buttons of bank
            bank_bits = GPIO_READ_BANK();
            current_time = READ_U32BIT_US_TIME(); 
            if(bank_bits) // nothing to do if no end buttons down
            {
                for (size_t position = 0; position < PS_NUMBER_OF_KEYS_PER_BANK; position++)
                {
                    int key = bank * position;
                    // if the end key is down
                    bool button_down = bank_bits & (1 << position);
                    
                    switch(key_data[key].state)
                    {
                        case PS_KEY_STATE_IDLE:
                            if(button_down)
                            {
                                // illegal state - something must be wrong - log error
                                PS_LOG_FMT("ERROR end detected before start: key:%i bank:%i, bit:%i", key, bank, position);
                            }
                            break;
                        case PS_KEY_STATE_STARTED:
                            if(button_down)
                            {
                                uint32_t duration = current_time - key_data[key].press_time;
                                key_data[key].state = PS_KEY_STATE_HIT;
                                PS_LOG_FMT("HIT key:%i bank:%i, bit:%i, duration:%lu", key, bank, position, duration);
                                // todo calc the velocity and queue the hit
                            }
                            break;
                        case PS_KEY_STATE_HIT:
                            // do nothing as we wait for start button to go back to idle
                            break;
                    }
                }
            }

            // clock shift register to next bank
            GPIO_HIGH(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);
            GPIO__LOW(PS_SHIFT_REG_CLOCK_GPIO_NUMBER);

        }
    }
}