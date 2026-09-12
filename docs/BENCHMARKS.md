# Benchmark evidence

## Measurement environment

- Date: 2026-09-07.
- CPU: 13th Gen Intel Core i5-1335U; 12 logical processors reported.
- OS for the Windows comparison suite: Windows 11, NT build 10.0.26200.
- Windows compiler/build tools: MSYS2 UCRT64 GCC 16.1.0, 64-bit; CMake 4.4.3 / Ninja 1.13.2.
- Additional Linux full-engine run: Ubuntu 22.04.5 under WSL2, GCC 11.4.0, CMake 3.22.1 / Make.
- Builds: `Release`, `-O3 -DNDEBUG`; no fast-math, LTO, or global native/AVX2 target flags. Only the AVX2 source uses `-mavx2`; floating-point contraction is disabled.
- Main thread consumes; one additional thread produces. OS scheduling is used, without affinity or CPU isolation. Laptop scheduling, power, temperature, and background activity can affect results; power settings were not controlled or recorded.

These are synthetic workload measurements on this machine. They do not establish performance on every CPU, realistic market-data distributions, profitable trading, or latency percentiles.

## Timed workload

Full engine and queue tests use **100,000,000 ticks per run**, **five runs**, and **1,000,000 untimed warmup ticks**. Report the middle run by elapsed time, with all raw runs retained. Risk-only tests use **1,000,000 calculations**, **five runs**, and **10,000 warmup calculations**.

The synthetic producer uses deterministic xorshift32 state and creates 48-byte events with prices around 100, a 0.02 bid/ask spread, positive reference liquidity, and 1,000 ns timestamp increments. These timestamps do not throttle replay. The queue holds 16,384 events, approximately 768 KiB of payload.

Full-engine configuration: 1,000,000 initial cash, ±10-share position target, a strategy decision every 64 ticks, 0.01 price-movement threshold, 0.5 bps slippage, 2 bps quadratic impact coefficient, 1,024 rolling returns, and a risk calculation every 256 ticks. The same configuration applies to scalar and AVX2 comparisons. The strategy generates orders on price signals; there is no artificial bypass of execution or risk work.

Timing starts immediately before the producer start gate is released and ends after the consumer drains the specified events and computes any outstanding final risk snapshot. Included: synthetic generation or in-memory replay, queue writes/reads, all full/empty waits, and the mode's consumer work. Excluded: allocation/initialization, thread construction/join, CSV parsing, warmup, and output. Queue-only mode consumes timestamps into a checksum and does not execute the trading or risk pipeline.

The focused risk benchmark updates a rolling equity observation and recomputes statistics over 1,024 returns each iteration. It includes return insertion, both statistical passes, final Sharpe/VaR formulas, and checksum accumulation. Its calculations/sec are not ticks/sec for the full backtester.

## Reproduce on Linux

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/backtester benchmark --ticks 100000000 --runs 5 --risk avx2
./build/backtester queue --ticks 100000000 --runs 5
./build/backtester benchmark --ticks 100000000 --runs 5 --risk scalar
./build/backtester risk --ticks 1000000 --warmup 10000 --runs 5 --risk scalar
./build/backtester risk --ticks 1000000 --warmup 10000 --runs 5 --risk avx2
```

Run benchmarks sequentially after builds and tests finish. Compare identical settings, keep all repetitions, and disclose your own machine and compiler. `--risk auto` selects AVX2 when supported; explicit AVX2 requests reject unsupported CPUs/builds.

## Reproduce in this Windows workspace

The ignored `.tools` directory contains the downloaded CMake and Ninja tooling. GCC is installed in MSYS2. These PowerShell commands use their actual locations:

```powershell
$btCmake = Join-Path $PWD '.tools\python\cmake\data\bin\cmake.exe'
$btCtest = Join-Path $PWD '.tools\python\cmake\data\bin\ctest.exe'
$btNinja = Join-Path $PWD '.tools\python\bin\ninja.exe'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
& $btCmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  '-DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe' "-DCMAKE_MAKE_PROGRAM=$btNinja"
& $btCmake --build build -j 4
& $btCtest --test-dir build --output-on-failure
& .\build\backtester.exe benchmark --ticks 100000000 --runs 5 --risk avx2
& .\build\backtester.exe queue --ticks 100000000 --runs 5
& .\build\backtester.exe benchmark --ticks 100000000 --runs 5 --risk scalar
& .\build\backtester.exe risk --ticks 1000000 --warmup 10000 --runs 5 --risk scalar
& .\build\backtester.exe risk --ticks 1000000 --warmup 10000 --runs 5 --risk avx2
```

Other checkouts need their own CMake/Ninja installation; `.tools` and build outputs are intentionally ignored.

## Results

Final Windows captures, after all compilation/testing completed:

| Workload | Median throughput | Range of five runs | Raw output |
|---|---:|---:|---|
| **Full engine, AVX2 risk** | **17.889 M ticks/sec** | 17.832–25.025 M | `benchmarks/full-avx2.txt` |
| Full engine, scalar risk | 11.880 M ticks/sec | 11.546–15.573 M | `benchmarks/full-scalar.txt` |
| Queue plus producer/checksum | 26.347 M ticks/sec | 23.023–59.601 M | `benchmarks/queue.txt` |

The full-engine result supports the **10M+ ticks/sec** claim for the stated workload and hardware. The queue number is deliberately separate. All runs are retained; the faster first runs and subsequent variation are visible. No claim is made that the observed differences isolate AVX2 alone: the laptop's operating conditions were not controlled.

The separate **Linux/WSL2 full-engine AVX2 run measured 31.230 M ticks/sec**, with a five-run range of **30.135–32.903 M** and a median elapsed time of **3.202027 seconds**. It used the same 100-million-tick workload, five repetitions, warmup, and engine settings, and produced the same printed final state. See `benchmarks/linux-full-avx2.txt`. The Linux and Windows toolchains, runtimes, and scheduling environments differ, so this is not an isolated comparison of operating systems. A defensible CV wording is: “Processed 31.2M synthetic ticks/sec in a two-thread full-engine benchmark on an i5-1335U, GCC 11.4 Release under WSL2; median of five 100M-tick runs.”

The median AVX2 full-engine run was 5.590009 seconds for 100,000,000 ticks. Its observed final state was:

```text
orders=934274 fills=934274 rejected=0 position=-10 risk_updates=390625
cash=407817.539491 equity=406817.980491 pnl=-593182.019509
realized=-593181.869483 unrealized=-0.150025
sharpe=-0.0127934816 var95=0.6971870946
observations=1024 skipped_returns=0
```

Scalar and AVX2 runs produced the same printed final state. This synthetic strategy loses money; that output demonstrates functioning execution/accounting rather than an investment result.

The focused Windows risk benchmark measured **149,058.614 calculations/sec** in scalar mode and **1,142,390.656 calculations/sec** in AVX2 mode (medians). Both printed checksum `1842264160.242374`. The five-run ranges were 137,782.978–286,240.904 and 574,327.844–1,190,187.712 calculations/sec respectively; this variation again limits isolated speedup attribution. See `benchmarks/risk-scalar.txt` and `benchmarks/risk-avx2.txt`. These are risk-window calculations, not full-engine tick throughput, and do not imply a fixed whole-program SIMD speedup.

Earlier 100-million-tick exploration is retained as `benchmarks/preliminary-full-avx2.txt`, `benchmarks/preliminary-full-scalar.txt`, and `benchmarks/preliminary-queue.txt`. Those captures preceded the final notional-overflow guard and output-label correction; the final table above uses the final executable. The preliminary queue timing label listed final risk even though that mode did no risk work; the final output corrects that label.

## Verification

- Windows GCC Release: all component, replay, and invalid-command tests passed. `benchmarks/tests-release.txt`.
- Windows GCC Debug: all three CTest entries passed. Manual checks confirmed Debug benchmark refusal and rejection of even repetition counts.
- Disassembly of the AVX2 object contains `vpermpd`, packed additions, and packed multiplications in the two risk-statistics reductions.
- `benchmarks/replay.txt` demonstrates fills and risk; its ten events are a functional check, not credible throughput evidence.

Linux validation used Ubuntu 22.04.5 under WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`, GCC 11.4.0, and CMake 3.22.1. No source changes were needed for Linux:

| Configuration | Result | Evidence |
|---|---|---|
| Release, AVX2 enabled | 3/3 CTest entries pass; both allocation checks pass | `benchmarks/linux-validation.txt` |
| Release, AVX2 omitted | 3/3 pass; expected SIMD-equivalence skip and scalar fallback | `benchmarks/linux-scalar-validation.txt` |
| Debug, AddressSanitizer + UndefinedBehaviorSanitizer | 3/3 pass, no sanitizer findings | `benchmarks/linux-asan-validation.txt` |
| Debug, ThreadSanitizer | 3/3 pass with the process-local WSL workaround below | `benchmarks/linux-tsan-no-aslr-validation.txt` |

Normal TSan startup on this WSL/GCC combination aborted with `unexpected memory mapping` before running tests (`benchmarks/linux-tsan-validation.txt`). Running `setarch x86_64 -R ctest --test-dir build-linux-tsan --output-on-failure` avoids that runtime mapping conflict for this process. No system ASLR settings were changed. The passing TSan run used this workaround; benchmark binaries did not.

Linux artifacts use separate `build-linux`, `build-linux-scalar`, `build-linux-asan`, and `build-linux-tsan` directories so they do not reuse the Windows CMake cache. GCC/Linux and GCC/Windows were tested; Clang and MSVC build paths are provided but were not run on this machine.
