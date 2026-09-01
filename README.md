# Ticker

[![PlatformIO Registry](https://badges.registry.platformio.org/packages/fyfar/library/Ticker.svg)](https://registry.platformio.org/libraries/fyfar/Ticker) ![license ISC](https://img.shields.io/badge/license-ISC-blue) ![contributions welcome](https://img.shields.io/badge/contributions-welcome-brightgreen)

Attach/detach callback timers for CH32V-family RISC-V microcontrollers, built directly on [ch32fun](https://github.com/cnlohr/ch32fun). No Arduino core, no HAL, no dynamic allocation. One `Ticker` type, independently named instances, the same shape as ESP32's `Ticker` class.

```ini
; platformio.ini
lib_deps = fyfar/Ticker
```

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

## Features

Why this instead of configuring TIM2 by hand, or another timer library:

- **One timer, any number of callbacks.** Attach up to `TICKER_MAX_TICKERS` independent, differently-timed callbacks without claiming a hardware timer per callback — there are only two on CH32V003 to begin with.
- **No drift, no catch-up storms.** Deadlines advance on a fixed grid laid down at `attach()` time, so a repeating ticker holds its long-term rate exactly, and a missed period resyncs to the present instead of firing repeatedly to work off a backlog.
- **No dynamic allocation.** No `malloc`, no `new`, nothing that can fragment or fail unpredictably on a 2 KB chip. Every ticker is a fixed-size static array slot.
- **A known, measured footprint.** One ticker costs 1088 B flash and 64 B RAM on CH32V003 (see [Flash and RAM](#flash-and-ram)) — worth checking against the space you actually have before wiring it in.
- **A familiar shape.** Same `attach`/`once`/`detach` API as ESP32's `Ticker` class, if you've used that before.

## Prerequisites

- A RISC-V GCC toolchain that can target `rv32ec`/`ilp32e` for CH32V003, or `rv32imac`/`ilp32` for the larger supported chips, for example `riscv64-unknown-elf-gcc` or `riscv-none-elf-gcc`. `ch32fun.mk` picks the first one it finds on your `PATH` automatically.
- GNU Make.
- [`ch32fun`](https://github.com/cnlohr/ch32fun) itself. This library builds directly against ch32fun's register headers and `SystemInit()`, the same way the bundled examples do. It only supports the ch32fun core: not the Arduino core, not WCH's standard peripheral library, not any other framework.

## Installation

### PlatformIO

Published on the [PlatformIO Registry](https://registry.platformio.org/libraries/fyfar/Ticker). Add one line to `platformio.ini`:

```ini
[env:genericCH32V003F4P6]
platform = https://github.com/Community-PIO-CH32V/platform-ch32v.git
board = genericCH32V003F4P6
framework = ch32v003fun

lib_deps = fyfar/Ticker
```

PlatformIO downloads and compiles it on the next build. Pin a version with `fyfar/Ticker@^0.1.0` if you'd rather not pick up future releases automatically, or install from the command line with `pio pkg install --library "fyfar/Ticker"`.

Three things to know:

- The CH32V platform is community-maintained and isn't in the PlatformIO registry, so it has to be given as a URL. Plain `platform = ch32v` only works on a machine where it already happens to be installed.
- `ch32fun` comes from the framework, so there is no submodule to manage and nothing else to configure.
- ch32fun requires a `funconfig.h` to exist in your project's own `src/`, even when it is completely empty. Builds fail confusingly without it.

Then `#include "ch32fun.h"` and `#include "Ticker.h"`. [`examples/pio_ticker_blink`](examples/pio_ticker_blink) is a complete, working project you can copy.

### By hand, with a Makefile

The Makefile examples in this repository use ch32fun directly, without PlatformIO:

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
- [`examples/pio_ticker_blink`](examples/pio_ticker_blink): the same blink, as a PlatformIO project rather than a Makefile one.

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

## Limitations

- **Minimum interval: 10 microseconds**, not the 1 microsecond the API accepts. That's a limit of the chip, not this library — no amount of code changes it. Below that floor, you still get a callback, just not at the rate you requested. Design around 10 us as the real floor.

  Each fire costs roughly 4.5 us of interrupt entry, dispatch and rearm — measured on a CH32V003 at 48 MHz and reproduced at both SysTick rates the chip offers (27 ticks at 6 MHz, 210 ticks at 48 MHz, the same 4.4 us either way). That's the cost that puts the floor at 10 us: below it the ISR can't cycle fast enough to hit the requested rate, and 5 us, 2 us and 1 us all deliver about 9.5 us instead, i.e. "as fast as this chip can go" rather than the interval you asked for. It doesn't show up as jitter or error above the floor, though — deadlines advance on a fixed grid, so measured against a 24 MHz crystal, every requested interval from 10 us up to 1 second comes back exact.

- **Maximum interval depends on your clock configuration.** Common cases on CH32V003:

  | Clock configuration | SysTick rate | Maximum interval |
  |---|---|---|
  | Default (48 MHz core, SysTick at HCLK/8) | 6 MHz | ~358 s (~6 min) |
  | `FUNCONF_SYSTICK_USE_HCLK=1` (SysTick at full core clock) | 48 MHz | ~44 s |

  General formula: `2^31 / SysTick rate in Hz`. A faster SysTick gives finer resolution per tick at the cost of a lower ceiling.

  The ceiling exists because deadlines are stored as a 32-bit tick count and compared as a *signed* delta so wraparound is handled correctly — that signed comparison is what caps it at half the 32-bit range, not all of it. There's no way around it other than picking a slower SysTick rate. If you need something longer, `attach*`/`once*` return `false` rather than clamping or silently firing at some unrelated rate, so chain a shorter interval and re-arm it from the callback instead of relying on one long-running ticker.

- **Resolution doesn't degrade as you approach that ceiling.** A tick is a tick regardless of how close the deadline is to wrapping, since the comparison logic is wraparound-safe rather than relying on values staying small.
- **Ten tickers by default, fixed at compile time.** Raise or lower it with `TICKER_MAX_TICKERS`; there's no dynamic growth. `attach()` and friends return `false` if the registry is already full, so check the return value if you're near the limit.
- **A repeating ticker holds its long-term rate, and skips missed periods rather than queueing them.** Deadlines advance on a fixed grid laid down when you called `attach*`, so the interrupt overhead above does not accumulate: the average rate stays exactly what you asked for, and a 1 kHz ticker does not lose time against a wall clock. Individual gaps between callbacks still vary by that overhead, so this is a locked long-term rate, not a locked gap. If a period is missed entirely, because a callback ran long or the interval is shorter than the ISR floor, the ticker resyncs to the present instead of firing repeatedly to work off a backlog. You never get a catch-up storm, and a ticker asked to run faster than the hardware allows degrades to simply running as fast as it can.
- **Global `Ticker` instances don't depend on C++ constructors running.** `ch32fun` gates `__attribute__((constructor))` support behind a flag that defaults off, so a `Ticker` declared at file scope works correctly either way: its zero-initialized state is already valid, and the real setup happens inside `attach()`, not the constructor.

## Issues and pull requests

Both are welcome, especially reports from real hardware. The clock-rate assumption mentioned under board support was reasoned through from documentation and a rule that holds on comparable chips, not confirmed on a board. If you've got one of the affected chips on your desk, a scope trace showing whether intervals come out correct is worth more than any amount of further reading on my end.

## License

ISC. See [LICENSE](LICENSE).
