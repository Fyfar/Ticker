# Ticker

![version](https://img.shields.io/badge/version-0.1.0-blue) ![contributions welcome](https://img.shields.io/badge/contributions-welcome-brightgreen)

Attach/detach callback timers for CH32V-family RISC-V microcontrollers, built directly on [ch32fun](https://github.com/cnlohr/ch32fun). No Arduino core, no HAL, no dynamic allocation. One `Ticker` type, independently named instances, the same shape as ESP32's `Ticker` class.

```cpp
#include "ch32fun.h"
#include "Ticker.h"

static Ticker blinker;

static void toggle(void *ctx)
{
	(void)ctx;
	static uint8_t state = 0;
	state ^= 1;
	funDigitalWrite(PD0, state ? FUN_HIGH : FUN_LOW);
}

int main()
{
	SystemInit();
	funGpioInitAll();
	funPinMode(PD0, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);

	blinker.attach_ms(250, toggle);

	while (1) {
		// blinker keeps firing from TIM2's interrupt no matter what
		// happens here
	}
}
```

A single hardware timer (TIM2) is multiplexed across every attached `Ticker` using an interrupt-driven, chained rearm scheme, the same approach the RP2040 and ESP32 SDKs use internally. TIM1 and SysTick are left alone, so your application can still use them for PWM or its own timekeeping.

Verified on real CH32V003 hardware against a 24 MHz crystal: intervals from 10 microseconds to 1 second are delivered exactly, with no accumulating drift.

## Prerequisites

- A RISC-V GCC toolchain that can target `rv32ec`/`ilp32e` for CH32V003, or `rv32imac`/`ilp32` for the larger supported chips, for example `riscv64-unknown-elf-gcc` or `riscv-none-elf-gcc`. `ch32fun.mk` picks the first one it finds on your `PATH` automatically.
- GNU Make.
- [`ch32fun`](https://github.com/cnlohr/ch32fun) itself. This library builds directly against ch32fun's register headers and `SystemInit()`, the same way the bundled examples do. It only supports the ch32fun core: not the Arduino core, not WCH's standard peripheral library, not any other framework.

## Installation

There's no package manager entry for this yet, on the Arduino Library Manager or the PlatformIO registry, since it isn't an Arduino library. Add it to a ch32fun project by hand:

1. Get `ch32fun` into your project. The examples here use a git submodule at `ch32fun/`.
2. Copy `src/Ticker.h` and `src/Ticker.cpp` into your project, or add this repository as a submodule and point your include path at its `src/` directory.
3. In your Makefile, add `Ticker.cpp` to `ADDITIONAL_C_FILES` and `src/` to `CFLAGS`, both after the `include .../ch32fun.mk` line. See the [examples](examples) for a working Makefile: setting `CFLAGS` before that include silently drops ch32fun's optimization flags.

## API

```cpp
bool attach(uint32_t seconds, TickerCallback cb, void *ctx = 0);
bool attach_ms(uint32_t ms, TickerCallback cb, void *ctx = 0);
bool attach_us(uint32_t us, TickerCallback cb, void *ctx = 0);

bool once(uint32_t seconds, TickerCallback cb, void *ctx = 0);
bool once_ms(uint32_t ms, TickerCallback cb, void *ctx = 0);
bool once_us(uint32_t us, TickerCallback cb, void *ctx = 0);

void detach();
```

`attach*` repeats until detached. `once*` fires a single time. All six return `false` if the ticker was already active, if the registry is full, or if the requested interval is longer than the [maximum below](#limitations), so check the return value if that matters to you. `TickerCallback` is `void(*)(void *ctx)`, a plain function pointer plus a context pointer, not `std::function`. There are no capturing lambdas here; pass `this` as `ctx` and cast it back inside the callback if you need to reach an object.

`detach()` is safe to call from inside any callback, including a ticker detaching itself.

## Examples

- [`examples/ticker_blink`](examples/ticker_blink): one repeating ticker, the smallest useful setup.
- [`examples/ticker_multi`](examples/ticker_multi): two independent tickers running at different rates.
- [`examples/ticker_oneshot`](examples/ticker_oneshot): a one-shot timeout alongside a repeating ticker that detaches itself from inside its own callback.

Each has a `Makefile` that expects the `ch32fun` submodule at `../../ch32fun` and this library's `src/` at `../../src`. Run `make TARGET_MCU=CH32V003` (or another supported chip) from inside the example directory.

## Board support

| Chips | Status | Why |
|---|---|---|
| CH32V003, CH32V002, CH32V004, CH32V005, CH32V006, CH32V007 | Supported | |
| CH32V10x, CH32V20x (all packages, including D8/D8W), CH32V30x, CH32L103, CH32X03x | Supported | |
| CH32H41x | Not supported | Incompatible clock and interrupt architecture |
| CH5xx family (CH551, CH552, CH570 to CH592) | Not supported | No TIM2 timer |

Unsupported chips fail at compile time with a clear error instead of building something that would misbehave on real hardware.

CH32V003 is verified on real hardware, against a 24 MHz external crystal and a logic analyzer, in both SysTick configurations the chip offers. A 500 ms repeating ticker measures 1.000 Hz and a 500 us one measures 1.000 kHz, and a full test suite covering interval accuracy, the rejection of out-of-range intervals, one-shot semantics, registry capacity and detaching a ticker from inside another ticker's callback passes in every clock configuration.

Timing on the internal HSI oscillator inherits that oscillator's own accuracy, roughly 1 percent, since it is an RC source rather than a crystal.

Timing accuracy on CH32V10x, plain CH32V20x, CH32V30x, and CH32L103 rests on a clock-rate assumption that hasn't been checked against real hardware yet. If intervals on one of these boards come out consistently off by a clean factor of two, that's the first thing to look at.

## Flash and RAM

Measured with GCC 16.1.0, `-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`, targeting CH32V003 (16 KB flash, 2 KB RAM), with every ticker using the same trivial callback so the numbers reflect ticker count rather than callback complexity:

| Configuration | Flash | RAM |
|---|---|---|
| Same sketch, no Ticker | 844 B (5.2%) | — |
| One ticker | 1088 B (6.6%) | 64 B (3.1%) |
| Two tickers | 1152 B (7.0%) | 84 B (4.1%) |
| Five tickers | 1260 B (7.7%) | 144 B (7.0%) |
| Ten tickers (the default cap) | 1300 B (7.9%) | 244 B (11.9%) |

`sizeof(Ticker)` is exactly 20 bytes: an interval, a deadline, a callback pointer, a context pointer, one flag byte and padding. The registry that tracks which tickers are active costs 4 bytes per slot, 40 bytes at the default cap of 10.

RAM is exactly `2 + 4 * TICKER_MAX_TICKERS + 20 * (number of Ticker objects)` bytes, rounded up to alignment, which reproduces every row above. Flash grows a lot less per ticker: about 230 bytes for the first one, then roughly 10 to 40 bytes for each one after that as the shared dispatch code gets reused. What actually drives your flash budget past that point is what your callbacks do, not how many tickers you attach.

If your project needs fewer than the default 10 tickers, `#define TICKER_MAX_TICKERS 4` (or whatever you need) before including `Ticker.h` to shave a few bytes off the registry. There's a compile-time check that keeps this value between 1 and 254.

## Impact on the chip

TIM2 is claimed exclusively by this library. Don't configure it elsewhere in a project that uses Ticker. TIM1 and SysTick are untouched, so PWM on TIM1 and `ch32fun`'s own delay functions keep working normally alongside it.

Ticker callbacks run inside TIM2's interrupt handler. Keep them short. A callback that blocks or runs long delays every other attached ticker, not just itself, since one interrupt dispatches everything that's currently due before rearming the timer.

There's no dynamic allocation anywhere in this library: no `malloc`, no `new`, nothing that could fragment or fail unpredictably on a 2 KB chip. Everything is a fixed-size static array and plain function pointers.

## Limitations

- **The practical floor is about 10 microseconds, not the 1 microsecond the API accepts.** Each fire costs roughly 4.5 us of interrupt entry, dispatch and rearm, measured on a CH32V003 at 48 MHz and reproduced at both SysTick rates the chip offers (27 ticks at 6 MHz, 210 ticks at 48 MHz, the same 4.4 us either way). Because deadlines advance on a fixed grid, that cost does not turn into interval error: measured against a 24 MHz crystal, requested intervals come back exact from 1 second all the way down to 10 us. Below that the ISR simply cannot cycle fast enough, and 5 us, 2 us and 1 us all deliver about 9.5 us instead. So 10 us is the shortest interval that means anything, and anything below it silently becomes "as fast as this chip can go".
- **The maximum interval depends on your clock configuration**, because deadlines are stored as a 32-bit tick count and compared as a *signed* delta so that wraparound is handled correctly. That signed comparison is what sets the ceiling: half the 32-bit range, not all of it. On CH32V003 at its default clock (48 MHz core, SysTick at an eighth of that, so 6 MHz), it works out to about 358 seconds, just under 6 minutes. If a project sets `FUNCONF_SYSTICK_USE_HCLK=1` to run SysTick at the full core clock instead, the ceiling drops to about 44 seconds, in exchange for finer resolution per tick. The formula is `2^31 / SysTick rate in Hz`. Anything longer is rejected: `attach*` and `once*` return `false` rather than clamping to the maximum or silently firing at some unrelated rate.
- **Resolution doesn't degrade as you approach that ceiling.** A tick is a tick regardless of how close the deadline is to wrapping, since the comparison logic is wraparound-safe rather than relying on values staying small.
- **Ten tickers by default, fixed at compile time.** Raise or lower it with `TICKER_MAX_TICKERS`; there's no dynamic growth. `attach()` and friends return `false` if the registry is already full, so check the return value if you're near the limit.
- **A repeating ticker holds its long-term rate, and skips missed periods rather than queueing them.** Deadlines advance on a fixed grid laid down when you called `attach*`, so the interrupt overhead above does not accumulate: the average rate stays exactly what you asked for, and a 1 kHz ticker does not lose time against a wall clock. Individual gaps between callbacks still vary by that overhead, so this is a locked long-term rate, not a locked gap. If a period is missed entirely, because a callback ran long or the interval is shorter than the ISR floor, the ticker resyncs to the present instead of firing repeatedly to work off a backlog. You never get a catch-up storm, and a ticker asked to run faster than the hardware allows degrades to simply running as fast as it can.
- **Global `Ticker` instances don't depend on C++ constructors running.** `ch32fun` gates `__attribute__((constructor))` support behind a flag that defaults off, so a `Ticker` declared at file scope works correctly either way: its zero-initialized state is already valid, and the real setup happens inside `attach()`, not the constructor.

## Issues and pull requests

Both are welcome, especially reports from real hardware. The clock-rate assumption mentioned under board support was reasoned through from documentation and a rule that holds on comparable chips, not confirmed on a board. If you've got one of the affected chips on your desk, a scope trace showing whether intervals come out correct is worth more than any amount of further reading on my end.

## License

MIT. See [LICENSE](LICENSE).
