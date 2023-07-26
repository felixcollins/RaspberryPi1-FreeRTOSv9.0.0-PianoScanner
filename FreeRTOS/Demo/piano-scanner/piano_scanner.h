#pragma once

#define PS_DEBUG_LOGGING 1

//#define PS_LOG(format, ... ) printf(format "\n\r", __VA_ARGS__)
/* #define PS_LOG(...) \
         do { if (PS_DEBUG_LOGGING) fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, \
           __LINE__, __func__, __VA_ARGS__); } while (0)
            */

#define PS_LOG_FMT(fmt, ...) \
            do { if (PS_DEBUG_LOGGING) fprintf(stderr, fmt "\n\r", __VA_ARGS__); } while (0)

// This sets the number of shifts done in the shift register starting from the first bit
#define PS_NUMBER_OF_KEY_BANKS 10

// This number needs to stay the same unless the defines below are also changed
#define PS_NUMBER_OF_KEYS_PER_BANK 8

// Uses R-Pi1 B+ V1.2 GPIO 2 to 9 to get consecutive bits in the gpio port
// If consecutive ports/bits are not available the port reading part of the code 
// will need to be re-written. The defines below also assume PS_NUMBER_OF_KEYS_PER_BANK is 8
#define PS_KEY_0_PORT_GPIO_NUMBER 2 
#define PS_KEY_7_PORT_GPIO_NUMBER 9
#define PS_KEY_PORT_MASK (0x000000FF << PS_KEY_0_PORT_GPIO_NUMBER)

// Control pins for MC595 Shift Registers
// Three devices are used daisy chained to provide 24 outputs
// A single bit is walked through the shift register to stimulate 
// the m/b lines for all the banks
#define PS_SHIFT_REG_RESET_GPIO_NUMBER 10
#define PS_SHIFT_REG_INPUT_GPIO_NUMBER 11
#define PS_SHIFT_REG_CLOCK_GPIO_NUMBER 12
#define PS_SHIFT_REG_LATCH_GPIO_NUMBER 13

#define LED_PIN 47

#define PS_STARTING_NOTE_MIDI_NUMBER 22

#define PS_DEBOUNCE_TIME_US 10000

// Abstraction of system calls
#define GPIO_HIGH(pin)  bcm2835_gpio_set(pin)
#define GPIO__LOW(pin)  bcm2835_gpio_clr(pin)
#define GPIO_READ_BANK() (bcm2835_peri_read(bcm2835_gpio + BCM2835_GPLEV0/4) & PS_KEY_PORT_MASK) >> PS_KEY_0_PORT_GPIO_NUMBER;  
#define READ_U32BIT_US_TIME() 	bcm2835_peri_read(bcm2835_st + BCM2835_ST_CLO/4);
#define RUN_LED_ON() bcm2835_gpio_set(LED_PIN)
#define RUN_LED_OFF() bcm2835_gpio_clr(LED_PIN)


void ps_init(void);
