# Capture Mode

This guide explains how to trace fiber execution and analyze call timings with the capture profiler.

Use capture mode when you need to investigate the calls made during a stall. It records call timings for sampled fiber executions and reports after the fiber switches away. For sampled backtraces while a stall is still in progress, see [Watchdog Mode](../watchdog-mode/index).

## Usage

Add `fiber-profiler` to your application's bundle as described in [Getting Started](../getting-started/index), then select capture mode when starting Async or Falcon:

```bash
FIBER_PROFILER=capture bundle exec falcon serve
```

Async starts and stops capture automatically. The legacy `FIBER_PROFILER_CAPTURE=true` setting also enables capture when `FIBER_PROFILER` is unset. An explicit `FIBER_PROFILER` value takes precedence.

### Manual Instrumentation

To reproduce a stall without a scheduler, save this as `capture.rb`. It starts a {ruby Fiber::Profiler::Capture} and simulates a blocking operation inside a fiber:

```ruby
require "fiber/profiler"

profiler = Fiber::Profiler::Capture.new

begin
	profiler.start
	
	# Simulate a blocking operation without a scheduler:
	Fiber.new(blocking: false) do
		sleep 0.1
	end.resume
ensure
	profiler.stop
end
```

```bash
bundle exec ruby capture.rb
```

This example starts the profiler explicitly, so it does not need `FIBER_PROFILER=capture`. The report should include `Kernel#sleep` and its elapsed duration. Start and stop profiling on the same thread.

## Configuration

Set these environment variables before loading `fiber-profiler`; the native extension reads them when it loads:

| Variable | Default | Meaning |
| --- | --- | --- |
| `FIBER_PROFILER_CAPTURE_STALL_THRESHOLD` | `0.01` | Minimum execution duration in seconds to exceed before reporting a stall. |
| `FIBER_PROFILER_CAPTURE_FILTER_THRESHOLD` | 10% of the stall threshold | Filter calls shorter than this duration in seconds. |
| `FIBER_PROFILER_CAPTURE_TRACK_CALLS` | `true` | Record call timings. Set to `false` to report stall durations without tracing calls. |
| `FIBER_PROFILER_CAPTURE_SAMPLE_RATE` | `1.0` | Fraction of eligible fiber executions to sample: `1.0` samples all, `0.1` samples approximately 10%. |

These settings apply to capture mode only. The sample rate controls selection at fiber switches; it is not a time interval between backtrace samples.

For explicit instrumentation, override the defaults with `Fiber::Profiler::Capture.new(stall_threshold: 0.05, filter_threshold: 0.005, track_calls: true, sample_rate: 0.1)`. Pass `output:` to select a writable IO; the default is standard error.

## Reading Reports

When standard error is a terminal, capture prints a readable call log. Redirected output uses one JSON object per stall, including the execution `duration` and a `calls` array. Each retained call includes its source location, class, method, duration, and nesting information.

To collect the example's reports and summarize call timings:

```bash
bundle exec ruby capture.rb 2> capture.ndjson
bundle exec bake input --file capture.ndjson fiber:profiler:analyze output
```

The analyzer aggregates call durations by source location and sorts the summary by total duration. Feed it capture reports; unrelated application output on standard error must be separated from the JSON first. With `track_calls: false`, reports contain no call timings to aggregate.

Call durations can include time spent in nested calls, so adding durations across different locations does not give total application runtime. Short or uninformative calls may be filtered from the report.

## Interpretation and Limits

Capture samples non-blocking fibers and excludes blocking fibers, including the event-loop fiber. It measures wall time between fiber switches, so an ordinary scheduler-aware wait that yields does not count its entire wait time as a stall. A blocking operation that keeps the same fiber executing can count toward a stall.

Reports are emitted when the fiber switches away. An operation that never yields or returns can therefore prevent its capture report from appearing. Use [Watchdog Mode](../watchdog-mode/index) to investigate an ongoing stall when Ruby thread scheduling is still possible.

Call tracing can substantially affect performance. Reduce the sample rate to trace fewer executions, or disable call tracking if you only need stall durations. Use the [Falcon overhead benchmark](https://github.com/socketry/fiber-profiler/tree/main/benchmark/falcon) to compare modes, and measure with your application's Ruby/JIT configuration and workload.
