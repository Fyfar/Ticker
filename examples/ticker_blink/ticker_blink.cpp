// Minimal Ticker usage: one repeating ticker toggles a GPIO. The main loop
// only busy-waits, demonstrating the callback fires from TIM2's ISR
// regardless of what loop() is doing.
#include "ch32fun.h"
#include "Ticker.h"

#define BLINK_PIN PD0

static Ticker blinker;

static void toggle_pin(void *ctx)
{
	(void)ctx;
	static uint8_t state = 0;
	state ^= 1;
	funDigitalWrite(BLINK_PIN, state ? FUN_HIGH : FUN_LOW);
}

int main()
{
	SystemInit();
	funGpioInitAll();
	funPinMode(BLINK_PIN, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);

	blinker.attach_ms(250, toggle_pin);

	while (1)
	{
		Delay_Ms(1000); // main loop busy; blinker keeps toggling regardless
	}
}
