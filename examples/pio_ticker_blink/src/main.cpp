// Minimal Ticker usage under PlatformIO: one repeating ticker toggles a GPIO
// while the main loop does something else entirely.
//
// Keep callbacks short. They run inside TIM2's interrupt handler with
// interrupts disabled, so anything slow there delays every other attached
// ticker. In particular do not call printf from a callback: ch32fun's debug
// printf waits on the host and will stall the chip from interrupt context.
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
