// Two independently-named Ticker instances running at different rates,
// proving they don't share state. Self-detach lives in ticker_oneshot.
#include "ch32fun.h"
#include "Ticker.h"

#define FAST_PIN PD0
#define SLOW_PIN PD4

static Ticker fastTick;
static Ticker slowTick;

static void fast_cb(void *ctx)
{
	(void)ctx;
	static uint8_t state = 0;
	state ^= 1;
	funDigitalWrite(FAST_PIN, state ? FUN_HIGH : FUN_LOW);
}

static void slow_cb(void *ctx)
{
	(void)ctx;
	static uint8_t state = 0;
	state ^= 1;
	funDigitalWrite(SLOW_PIN, state ? FUN_HIGH : FUN_LOW);
}

int main()
{
	SystemInit();
	funGpioInitAll();
	funPinMode(FAST_PIN, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
	funPinMode(SLOW_PIN, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);

	fastTick.attach_ms(50, fast_cb);
	slowTick.attach_ms(500, slow_cb);

	while (1)
	{
		asm volatile("nop");
	}
}
