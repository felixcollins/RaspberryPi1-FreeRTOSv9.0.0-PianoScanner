#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#include <stdio.h>

#include "bcm2835.h"
#include "bcm2835_irq.h"
#include "bcm2835_systimer.h"
#include "bcm2835_miniuart.h"
#include "raspberrypi1.h"

#include "piano_scanner.h"



int main (void) {
	/* Initialize the bcm2835 lib */
	bcm2835_init();
	/* Initialize the miniuart (Otherwise printf doesn't work) */
	bcm2835_miniuart_open();

	bcm2835_gpio_fsel(LED_PIN, BCM2835_GPIO_FSEL_OUTP);

	RUN_LED_ON();

	/* Send message */
	printf("Welcome to Piano Scanner\n\r");

	ps_init();

	vTaskStartScheduler();

	/*
	 *	We should never get here, but just in case something goes wrong,
	 *	we'll place the CPU into a safe loop.
	 */
	while(1);
}
