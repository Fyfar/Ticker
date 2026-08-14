#include "Ticker.h"

// GCC emits an __cxa_atexit registration for every global Ticker, since the
// destructor makes it non-trivial. This is freestanding, -nostdlib code with
// no C++ runtime and firmware that never exits, so stub both out instead of
// failing to link. Local-scope destruction (plain compiler-generated code at
// each return point) is untouched by this; it only affects the registration
// path for globals.
//
// Weak, because these are the C++ runtime's names, not this library's: an
// application that also supplies them (its own stubs, or a real runtime)
// would otherwise get a duplicate-symbol link failure pointing at Ticker.
extern "C" __attribute__((weak)) int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
extern "C" { __attribute__((weak)) void *__dso_handle = 0; }

// TIM2 and SysTick share a clock, so "ticks" is one unit end to end: no
// conversion between funSysTick32() ("now") and the value loaded into
// TIM2->ATRLR.
//
// TIM2's input is HCLK on every board below, by two different routes,
// checked against ch32fun.c's actual SystemInit():
//   - CH32V003, its V00x siblings (ch32fun.mk defines CH32V00x=1 for
//     CH32V002 too, so this covers it without a separate check), CH32X03x
//     without PLL, and CH32V20x: APB1 is never divided, so HCLK reaches
//     TIM2 directly.
//   - CH32V10x, CH32V30x, CH32L103, and CH32X03x with PLL forced on: APB1 =
//     HCLK/2, but the standard STM32 rule (a timer's clock doubles its APB
//     clock whenever that APB is prescaled) puts TIM2 back at HCLK. This
//     half of the table is inferred from that rule, not confirmed against
//     WCH's reference manual or real hardware. If timing on one of these
//     boards is off by a fixed ratio (2x, say), check this first.
#if defined(CH32V003) || defined(CH32V00x) || defined(CH32X03x) \
 || defined(CH32V10x) || defined(CH32V20x) || defined(CH32V30x) || defined(CH32L103)
	// SysTick runs at either HCLK or HCLK/8, decided by a condition in
	// ch32fun.h that has its own exceptions (CH32V10x opts out of the HCLK
	// case). Read that decision back out of DELAY_US_TIME rather than
	// restating the condition here, so the two can never drift apart. A
	// custom FUNCONF_PLL_MULTIPLIER needs no branch either: it moves HCLK's
	// absolute value, and SysTick and TIM2 scale with it identically.
	#define TICKER_TIM2_PSC (DELAY_US_TIME == (FUNCONF_SYSTEM_CORE_CLOCK / 1000000) ? 0 : 7)
#elif defined(CH32H41x)
	#error "Ticker: CH32H41x is not supported: it uses RCC_HB1Periph_TIM2 (a different bus scheme than the APB1 scheme this library assumes) and a different TIM2_IRQn."
#else
	#error "Ticker: unrecognized or unsupported target MCU. If this is a CH5xx-family board, it has no TIM2 peripheral and cannot support this library. If this is a new ch32fun-supported board, TICKER_TIM2_PSC needs an entry derived from its actual SystemInit() clock-tree config."
#endif

// CH32V00x siblings (V002/004-007) route through ch32fun's ch32x00xhw.h
// instead of ch32v003hw.h. Its TIM2 layout matches TIM_TypeDef field for
// field and the RCC bit matches too, but the header is missing
// TIM_CEN/TIM_UIE/TIM_UG/TIM_UIF and doesn't alias APB1PRSTR the way it
// aliases everything else. Every other chip header this library targets
// defines those four macros at 0x0001, so assume the same here; if wrong,
// TIM2 just never generates interrupts, rather than a subtle timing bug.
// Guarded individually so that ch32fun adding any of them upstream takes
// over silently instead of colliding.
#if defined(CH32V00x)
	#ifndef TIM_CEN
	#define TIM_CEN ((uint16_t)0x0001)
	#endif
	#ifndef TIM_UIE
	#define TIM_UIE ((uint16_t)0x0001)
	#endif
	#ifndef TIM_UG
	#define TIM_UG  ((uint16_t)0x0001)
	#endif
	#ifndef TIM_UIF
	#define TIM_UIF ((uint16_t)0x0001)
	#endif
	#define TICKER_APB1PRSTR PB1PRSTR // ch32x00xhw.h's one unaliased RCC field
#else
	#define TICKER_APB1PRSTR APB1PRSTR
#endif

#define TICKER_MAX_WINDOW 0xFFFEu // TIM2 is a 16-bit counter

// A deadline is compared against "now" as a signed 32-bit delta, so the
// representable horizon is 2^31 ticks, not 2^32: anything further out reads
// as already overdue and the ticker fires on every timer tick forever.
// Ticks_from_Ms/Us multiply in uint32_t and wrap silently past 2^32 as well,
// which would arm some unrelated short interval instead.
//
// Both are rejected at the API surface, before the multiply, rather than
// clamped: a timer that quietly runs at a rate you didn't ask for is worse
// than one that tells you it can't. These are all compile-time constants, so
// each check costs one compare against an immediate.
#define TICKER_MAX_TICKS 0x7FFFFFFFu
#define TICKER_MAX_MS    (TICKER_MAX_TICKS / DELAY_MS_TIME)
#define TICKER_MAX_US    (TICKER_MAX_TICKS / DELAY_US_TIME)
#define TICKER_MAX_S     (TICKER_MAX_MS / 1000u)

// The whole point of the limits above is that a value passing the check
// cannot overflow the conversion behind it. Redo that multiply in 64-bit
// arithmetic, at compile time, so an edit that breaks the relationship fails
// the build instead of shipping a timer that fires at the wrong rate.
static_assert((unsigned long long)TICKER_MAX_MS * DELAY_MS_TIME <= TICKER_MAX_TICKS
           && (unsigned long long)TICKER_MAX_US * DELAY_US_TIME <= TICKER_MAX_TICKS
           && (unsigned long long)TICKER_MAX_S * 1000ull * DELAY_MS_TIME <= TICKER_MAX_TICKS,
	"TICKER_MAX_* admits an interval that overflows the tick conversion");

static Ticker *s_registry[TICKER_MAX_TICKERS];
static uint8_t s_engine_started;

// True only while TIM2_IRQHandler is running. A self-detach (or any
// arm()/detach() called from a callback) would otherwise rearm TIM2 once
// from inside the callback and again from the ISR's own tail call, a few
// instructions apart. The tail call already reschedules after every
// callback runs, so arm()/detach() skip their own call while this is set.
static uint8_t s_in_isr;

// A ticker callback runs inside TIM2_IRQHandler, where trap entry has
// already cleared mstatus.MIE. Restoring only what was on before this scope
// started, rather than enabling unconditionally, matters for the same
// reason: an unconditional enable at scope exit would turn interrupts back
// on before the ISR itself returns.
struct TickerCriticalSection
{
	uint8_t was_enabled;
	TickerCriticalSection()
	{
		// Touches only mstatus.MIE (bit 0x08), deliberately not ch32fun's
		// __disable_irq()/__enable_irq(), which also touch MPIE (bit 0x80).
		// MPIE is hardware's own save slot for what this trap's mret will
		// restore MIE to. A self-detach constructs this class a second time
		// while already inside TIM2_IRQHandler's trap; clearing MPIE there
		// corrupts that slot, and the outer mret then restores MIE to 0 for
		// good. Confirmed on real CH32V003 hardware: TIM2 keeps counting,
		// but nothing ever dispatches its interrupt again.
		//
		// The "memory" clobbers here are also what make the shared state
		// below safe without volatile: they bracket every registry access
		// the main line makes against the ISR's.
		uint32_t prior;
		__ASM volatile( ADD_ARCH_ZICSR "csrrc %0, mstatus, %1" : "=r"( prior ) : "r"( 0x08 ) : "memory" );
		was_enabled = ( prior & 0x08 ) != 0;
	}
	~TickerCriticalSection()
	{
		if ( was_enabled ) __ASM volatile( ADD_ARCH_ZICSR "csrrs zero, mstatus, %0" : : "r"( 0x08 ) : "memory" );
	}
};

void Ticker::engine_start(void)
{
	if (s_engine_started) return;
	s_engine_started = 1;

	RCC->APB1PCENR |= RCC_APB1Periph_TIM2; // aliased to PB1PCENR on CH32V00x siblings, no shim needed
	RCC->TICKER_APB1PRSTR |= RCC_APB1Periph_TIM2;
	RCC->TICKER_APB1PRSTR &= ~RCC_APB1Periph_TIM2;

	TIM2->PSC = TICKER_TIM2_PSC;
	TIM2->DMAINTENR |= TIM_UIE;
	NVIC_EnableIRQ(TIM2_IRQn);
}

// Reprograms TIM2 for the soonest active deadline, chaining through
// TICKER_MAX_WINDOW-sized hops when it's further out than the 16-bit
// counter covers. Caller must hold the critical section (or be
// TIM2_IRQHandler itself, already non-reentrant).
void Ticker::engine_rearm(void)
{
	// Stopped first: engine_rearm() can run twice per ISR pass (once from a
	// mid-loop self-detach, once from the ISR's own tail call), and starting
	// each call from a fully stopped counter keeps every call a clean,
	// self-contained cycle instead of one racing what the other just started.
	TIM2->CTLR1 &= ~TIM_CEN;

	uint32_t now = funSysTick32();
	uint32_t soonest_delta = 0xFFFFFFFFu;

	for (uint8_t i = 0; i < TICKER_MAX_TICKERS; i++)
	{
		Ticker *t = s_registry[i];
		if (!t) continue;
		int32_t delta = TimeElapsed32(t->deadline_ticks, now);
		// Overdue deadlines clamp to 1, never to 0. Zero does not mean "fire
		// immediately" on this timer, it means "stop": with ATRLR at zero the
		// counter freezes and no further update event is ever generated, so
		// the engine would never rearm again and every attached ticker would
		// go silent for good. Measured on CH32V003, where a single overdue
		// deadline killed a 10 ms ticker permanently. One is the smallest
		// reload that still means "on the next tick".
		uint32_t d = delta > 0 ? (uint32_t)delta : 1;
		if (d < soonest_delta) soonest_delta = d;
	}

	// Every real delta lands in [1, TICKER_MAX_TICKS], so the initial value
	// is unreachable and doubles as "no active tickers".
	if (soonest_delta == 0xFFFFFFFFu) return; // leave TIM2 stopped

	if (soonest_delta > TICKER_MAX_WINDOW) soonest_delta = TICKER_MAX_WINDOW;

	TIM2->ATRLR = (uint16_t)soonest_delta;
	TIM2->SWEVGR |= TIM_UG; // forces PSC/ATRLR to load now and resets the prescaler, which also sets UIF
	// CH32V003's UG does NOT reinitialize the counter, unlike the STM32 timer
	// this peripheral otherwise mirrors. Measured on hardware: write CNT=12345,
	// pulse UG, read back 12345. So zero it here, after the event rather than
	// before, which is correct on both behaviours.
	//
	// A stale CNT left above ATRLR means the counter is already past its
	// reload target the moment CEN goes high, and the interrupt lands at an
	// arbitrary time. That is invisible while ATRLR is TICKER_MAX_WINDOW,
	// since a 16-bit counter can barely exceed 0xFFFE, and wrong for every
	// interval short enough to fit in a single window.
	TIM2->CNT = 0;
	// Clear-on-write-0, not write-1: writing ~TIM_UIF clears only that bit
	// and leaves every other flag alone.
	TIM2->INTFR = (uint16_t)~TIM_UIF;
	// The forced update above also latches TIM2's line pending in the PFIC,
	// separately from INTFR/UIF above. Left set, every rearm() re-triggers
	// the instant this ISR returns, regardless of ATRLR. Confirmed on real
	// hardware at ~130,000 interrupts/sec instead of the intended ~91/sec,
	// which starves everything outside the ISR.
	NVIC_ClearPendingIRQ(TIM2_IRQn);
	TIM2->CTLR1 |= TIM_CEN;
}

bool Ticker::arm(uint32_t ticks, TickerCallback c, void *context, bool repeat)
{
	if (!c || ticks == 0 || active) return false;

	TickerCriticalSection cs;

	uint8_t slot = TICKER_MAX_TICKERS;
	for (uint8_t i = 0; i < TICKER_MAX_TICKERS; i++)
	{
		if (!s_registry[i]) { slot = i; break; }
	}
	if (slot == TICKER_MAX_TICKERS) return false; // registry full; cs restores IRQ state on the way out, TIM2 left unclocked

	engine_start();

	cb = c;
	ctx = context;
	interval_ticks = repeat ? ticks : 0;
	deadline_ticks = funSysTick32() + ticks;
	active = 1;

	s_registry[slot] = this;
	if (!s_in_isr) engine_rearm(); // the ISR's own tail call reschedules otherwise

	return true;
}

void Ticker::detach()
{
	if (!active) return; // unsynchronized peek: worst case is one redundant lock, never corrupts state

	TickerCriticalSection cs;
	if (active)
	{
		for (uint8_t i = 0; i < TICKER_MAX_TICKERS; i++)
		{
			if (s_registry[i] == this) { s_registry[i] = 0; break; }
		}
		active = 0;
		if (!s_in_isr) engine_rearm(); // the ISR's own tail call reschedules otherwise
	}
}

bool Ticker::attach(uint32_t seconds, TickerCallback c, void *context) { return seconds <= TICKER_MAX_S  && arm(Ticks_from_Ms(seconds * 1000u), c, context, true); }
bool Ticker::attach_ms(uint32_t ms, TickerCallback c, void *context)   { return ms      <= TICKER_MAX_MS && arm(Ticks_from_Ms(ms), c, context, true); }
bool Ticker::attach_us(uint32_t us, TickerCallback c, void *context)   { return us      <= TICKER_MAX_US && arm(Ticks_from_Us(us), c, context, true); }
bool Ticker::once(uint32_t seconds, TickerCallback c, void *context)   { return seconds <= TICKER_MAX_S  && arm(Ticks_from_Ms(seconds * 1000u), c, context, false); }
bool Ticker::once_ms(uint32_t ms, TickerCallback c, void *context)     { return ms      <= TICKER_MAX_MS && arm(Ticks_from_Ms(ms), c, context, false); }
bool Ticker::once_us(uint32_t us, TickerCallback c, void *context)     { return us      <= TICKER_MAX_US && arm(Ticks_from_Us(us), c, context, false); }

// extern "C": the vector table (built in plain C ch32fun.c) binds this weak
// symbol by its unmangled name, so without extern "C" this override would
// be silently ignored and TIM2 interrupts would hit DefaultIRQHandler instead.
extern "C" void TIM2_IRQHandler(void) INTERRUPT_DECORATOR;
extern "C" void TIM2_IRQHandler(void)
{
	TIM2->INTFR = (uint16_t)~TIM_UIF; // clear-on-write-0; see engine_rearm()

	s_in_isr = 1; // callbacks below must not rearm on their own; see s_in_isr above

	uint32_t now = funSysTick32();

	for (uint8_t i = 0; i < TICKER_MAX_TICKERS; i++)
	{
		Ticker *t = s_registry[i];
		if (!t) continue;
		if (TimeElapsed32(now, t->deadline_ticks) < 0) continue; // not due yet

		TickerCallback c = t->cb;
		void *context = t->ctx;

		if (t->interval_ticks) // nonzero interval is what "repeating" means
		{
			// Advance on the grid laid down at attach time, not from the
			// moment this callback happens to run. Rescheduling from "now"
			// silently adds the interrupt entry latency to every period, a
			// measured 4.5 us on CH32V003, which compounds without limit:
			// 0.9 percent low at a 500 us interval, and about 16 seconds an
			// hour at 1 kHz.
			t->deadline_ticks += t->interval_ticks;

			// ...unless that still leaves the deadline in the past, meaning a
			// whole period was missed. Grid-advancing a ticker that cannot
			// keep up accumulates a backlog it can never work off, and the
			// engine would then run the ISR flat out forever, starving
			// everything else. Resyncing gives up the grid exactly when the
			// requested rate is unachievable, which is also exactly when the
			// grid was a fiction. Missed periods are skipped, never queued.
			if (TimeElapsed32(now, t->deadline_ticks) >= 0)
				t->deadline_ticks = now + t->interval_ticks;
		}
		else
		{
			s_registry[i] = 0;
			t->active = 0;
		}

		c(context); // never null: arm() rejects that, and only armed tickers are in the registry
	}

	s_in_isr = 0;
	Ticker::engine_rearm();
}
