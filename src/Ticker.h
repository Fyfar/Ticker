#pragma once
// Ticker: attach/detach callback timers for CH32V003 (ch32fun), multiplexed
// onto a single TIM2.

#include "ch32fun.h"

#ifndef TICKER_MAX_TICKERS
#define TICKER_MAX_TICKERS 10
#endif
// 255 would collide with the "slot not found" sentinel in Ticker.cpp and
// wrap the ISR's scan loop forever, so catch it at compile time.
static_assert(TICKER_MAX_TICKERS > 0 && TICKER_MAX_TICKERS < 255,
	"TICKER_MAX_TICKERS must be in (0, 255)");

typedef void (*TickerCallback)(void *ctx);

extern "C" void TIM2_IRQHandler(void);

class Ticker
{
public:
	Ticker() : cb(0), ctx(0), interval_ticks(0), deadline_ticks(0), active(0) {}

	// Detaches, so a Ticker going out of scope while still active (e.g.
	// stack-local) doesn't leave a dangling pointer in the ISR's registry.
	// This runs via ordinary scope-exit, not the .init_array/global-
	// constructor path FUNCONF_SUPPORT_CONSTRUCTORS gates, so it always runs.
	~Ticker() { detach(); }

	// A copy would leave two objects disagreeing about which one the
	// registry points at, so copying is disabled rather than left broken.
	Ticker(const Ticker &) = delete;
	Ticker &operator=(const Ticker &) = delete;

	// Repeating. Interval is integer-only (no float, keeps soft-float
	// routines out of the link). Returns false if already active, if the
	// registry is full, or if the interval exceeds what a 32-bit tick
	// deadline can express (see TICKER_MAX_* in Ticker.cpp; roughly 358 s at
	// the CH32V003 default clock, 44 s with FUNCONF_SYSTICK_USE_HCLK).
	bool attach(uint32_t seconds, TickerCallback cb, void *ctx = 0);
	bool attach_ms(uint32_t ms, TickerCallback cb, void *ctx = 0);
	bool attach_us(uint32_t us, TickerCallback cb, void *ctx = 0);

	// One-shot.
	bool once(uint32_t seconds, TickerCallback cb, void *ctx = 0);
	bool once_ms(uint32_t ms, TickerCallback cb, void *ctx = 0);
	bool once_us(uint32_t us, TickerCallback cb, void *ctx = 0);

	// Cancels this ticker. Safe from any callback, including its own
	// (self-detach) or another ticker's.
	void detach();

private:
	friend void TIM2_IRQHandler(void);

	TickerCallback cb;
	void *ctx;
	uint32_t interval_ticks; // 0 for one-shot; nonzero is what "repeating" means
	uint32_t deadline_ticks; // absolute tick (funSysTick32() domain)
	uint8_t  active;

	bool arm(uint32_t ticks, TickerCallback c, void *context, bool repeat);

	static void engine_start(void);
	static void engine_rearm(void);
};
