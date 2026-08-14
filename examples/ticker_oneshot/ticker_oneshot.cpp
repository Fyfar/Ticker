// once_ms one-shot, plus a repeating ticker that detaches itself from
// within its own callback once a target count is reached.
#include "ch32fun.h"
#include "Ticker.h"

#define LED_PIN PD0

static Ticker blinker;
static Ticker timeout;

static void blink_cb(void *ctx)
{
	Ticker *self = (Ticker *)ctx;
	static uint8_t state = 0;
	static uint8_t count = 0;

	state ^= 1;
	funDigitalWrite(LED_PIN, state ? FUN_HIGH : FUN_LOW);

	if (++count >= 10) // 5 full blinks, then stop itself
		self->detach();
}

static void timeout_cb(void *ctx)
{
	(void)ctx;
	// Fires exactly once, ~3s after boot, independent of blinker above.
	funDigitalWrite(LED_PIN, FUN_LOW);
}

int main()
{
	SystemInit();
	funGpioInitAll();
	funPinMode(LED_PIN, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);

	blinker.attach_ms(200, blink_cb, &blinker); // ctx = self, for self-detach
	timeout.once_ms(3000, timeout_cb);

	while (1)
	{
		asm volatile("nop");
	}
}
