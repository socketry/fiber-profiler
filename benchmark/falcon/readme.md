# Falcon Profiling Overhead

This benchmark compares disabled, watchdog, and capture modes using one Falcon worker and a separate ApacheBench (`ab`) process over loopback HTTP/1 with persistent connections.

## Running

Install Falcon and ApacheBench, and build this checkout's native extension (`bundle exec bake build`). Falcon is a benchmark dependency, not a runtime dependency of the profiler. Run with a Ruby environment that can load Falcon:

```sh
ROUNDS=5 DURATION=5 WARMUP=2 CONCURRENCY=32 ruby benchmark/falcon/run.rb > results.ndjson
```

`AB` can select a different ApacheBench executable. `DURATION` and `WARMUP` are seconds per run. Use an ordinary release build without coverage or sanitizers, and avoid competing workloads while measuring. If using Bundler, include Falcon in the benchmark application's bundle.

To compare with YJIT enabled, run the same command with `RUBYOPT=--yjit`. Run configurations sequentially so they do not compete for CPU. The output records whether YJIT was enabled in the server.

The runner starts a fresh server for each measurement and loads the profiler from this checkout's `lib` and `ext` directories. It clears inherited `FIBER_PROFILER*` settings and selects one mode, leaving each mode's configuration at its defaults. Mode order rotates between rounds to reduce order bias. Warmup precedes every measured run.

Two synthetic Rack workloads exercise different scheduling patterns:

- `/json`: build and serialize 25 product records, without explicit IO waits.
- `/io`: the same work with two cooperative 1 ms sleeps, representing short external waits without a database dependency.

Both use Falcon's Rack middleware, with the response cache disabled. There is no TLS, request logging, Rails application, or multi-worker supervisor. These are controlled comparisons of request processing and scheduling overhead, not a production capacity estimate.

## Measurements

Each output line records one measured run, including runtime/gem versions and the actual profiler class:

- Completed requests, requests/second, mean latency, and p95/p99 latency. ApacheBench reports percentile latencies at millisecond resolution. This is a closed-loop saturation test, so latency depends on achieved throughput; it does not measure latency at a fixed offered rate.
- Server process CPU time per completed request, including the sampler thread. Snapshots bracket the load-generator process, so they also include a small amount of startup/shutdown coordination.
- Ruby allocations per request and GC count. Native capture allocations are not included in Ruby's allocation counter.
- Report count during measurement and total diagnostic log bytes, including startup and warmup. Report output goes to a temporary file rather than a terminal.

The runner rejects failed/non-2xx requests, watchdog errors, and unsuccessful server exits. Server processes and temporary logs are cleaned up between runs.

## Recorded Results

Measured on 2026-09-27: Apple M4 Pro, macOS 27.0 (26A428), Ruby 4.0.7, Falcon 0.57.0, Async 2.46.0, io-event 1.22.1, Rack 3.2.7, JSON 3.0.2, ApacheBench 2.3 (revision 1923142). The native extension used a normal build. Each configuration ran five times, with 32 concurrent persistent connections, 2 seconds of warmup and 5 seconds of measurement per run. YJIT-disabled and YJIT-enabled batches ran sequentially.

Values below are medians of five runs; throughput also includes the observed minimum–maximum range. Raw measurements: [YJIT disabled](results-ruby-4.0.7.ndjson), [YJIT enabled](results-ruby-4.0.7-yjit.ndjson).

| YJIT | Workload | Profiler | Requests/s (range) | Server CPU µs/request | p95 ms |
| --- | --- | --- | --- | --- | --- |
| Disabled | json | Disabled | 18,930 (17,931–19,765) | 51.9 | 2 |
| Disabled | json | Watchdog | 18,681 (17,595–19,028) | 52.8 | 2 |
| Disabled | json | Capture | 5,399 (5,170–5,563) | 181.9 | 6 |
| Disabled | io | Disabled | 12,519 (12,043–12,748) | 64.8 | 3 |
| Disabled | io | Watchdog | 12,244 (11,874–12,657) | 66.0 | 3 |
| Disabled | io | Capture | 3,439 (3,270–3,479) | 287.2 | 10 |
| Enabled | json | Disabled | 30,410 (30,090–31,619) | 32.4 | 1 |
| Enabled | json | Watchdog | 29,583 (28,893–30,021) | 33.2 | 1 |
| Enabled | json | Capture | 3,934 (3,819–3,958) | 253.9 | 9 |
| Enabled | io | Disabled | 14,035 (13,507–14,040) | 42.9 | 3 |
| Enabled | io | Watchdog | 13,940 (13,514–14,085) | 45.2 | 3 |
| Enabled | io | Capture | 1,892 (1,813–1,944) | 527.6 | 20 |

Across these four configurations, watchdog median throughput was 0.7–2.7% below disabled mode, with 1.7–5.3% more server CPU time per request. Capture median throughput was 71–87% below disabled mode. Percentages compare the unrounded medians in the raw files.

Watchdog and disabled throughput ranges overlap in three of the four comparisons. Five short runs on a shared development machine cannot establish a precise overhead budget. Watchdog was substantially cheaper than capture here, but validate continuous use against your application's latency and CPU budgets, with its actual Ruby/JIT configuration and fiber-switch frequency.

There were no watchdog reports and two capture reports during the timed runs. These measurements primarily characterize instrumentation overhead; report-heavy workloads and slow output destinations need separate measurement. Ruby allocation counts, GC counts, mean/p99 latency, and report counts are retained in the raw files.
