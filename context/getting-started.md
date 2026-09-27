# Getting Started

This guide explains how to detect stalls using the fiber profiler.

## Installation

Add the gem to your project:

```bash
$ bundle add fiber-profiler
```

## Usage

Select the profiling mode when starting your application:

```bash
FIBER_PROFILER=watchdog bundle exec falcon serve
```

With Async, including Falcon, the scheduler starts and stops the selected profiler automatically. Include `fiber-profiler` in your application's bundle; no initializer or per-request instrumentation is needed.

| Setting | Behaviour |
| --- | --- |
| `FIBER_PROFILER=watchdog` | Periodically sample stacks during sustained fiber execution. |
| `FIBER_PROFILER=capture` | Trace calls and report fibers that exceed the capture threshold. |
| `FIBER_PROFILER=false` | Disable the default profiler, even if the legacy capture flag is enabled. |
| `FIBER_PROFILER` unset | Preserve the existing `FIBER_PROFILER_CAPTURE=true` behaviour. |

Unknown modes raise `ArgumentError`. Set environment variables before loading the gem: the native capture settings are read when the extension loads. The new mode selector and watchdog settings are read when `Fiber::Profiler.default` is called.

### Manual Instrumentation

Instrument your code using the default profiler:

```ruby
#!/usr/bin/env ruby

require "fiber/profiler"

profiler = Fiber::Profiler.default

begin
	profiler&.start
	
	# Your application code:
	Fiber.new do
		sleep 0.1
	end.resume
ensure
	profiler&.stop
end
```

Running this program will output the following:

```bash
$ FIBER_PROFILER_CAPTURE=true bundle exec ./test.rb
Fiber stalled for 0.105 seconds
/Users/samuel/Developer/socketry/fiber-profiler/test.rb:11 in c-call 'Kernel#sleep' (0.105s)
```

## Integration with Async

The fiber profiler is optionally supported by `Async`. Enable either mode using `FIBER_PROFILER=watchdog` or `FIBER_PROFILER=capture`. The legacy `FIBER_PROFILER_CAPTURE=true` setting continues to enable capture mode when `FIBER_PROFILER` is unset.

Each scheduler obtains its own profiler. Watchdog binds to the thread when profiling starts, so schedulers in separate worker processes or threads are monitored independently. After `fork`, inherited profiling is stopped in the child; a new scheduler starts a new profiler normally.

## Watchdog Mode

Watchdog uses a thread-specific `:fiber_switch` tracepoint to track which fiber is executing. A separate Ruby thread periodically captures the monitored thread's backtrace. It does not install method-call tracing hooks or require a heartbeat task.

By default, it samples every 100 ms, retains at most five recent stacks, and reports when the same fiber execution lasts at least 500 ms. A continued stall produces at most one report per additional threshold interval. Returning to the event loop or switching to another fiber resets the sample window. This measures individual uninterrupted fiber executions, rather than starvation of a scheduled heartbeat.

To reproduce a stall, save this as `stall.rb`:

```ruby
require "async"

Sync do
	deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 2
	while Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
	end
end
```

```bash
FIBER_PROFILER=watchdog bundle exec ruby stall.rb
```

The reports should point to the busy loop. Replacing the loop with `sleep(2)` allows Async to keep scheduling fibers and should produce no reports.

Terminal output contains readable stacks. Redirected output uses one JSON object per report, with `mode: "watchdog"`, process/thread/fiber identifiers, elapsed `duration`, and a bounded `samples` array. Each sample contains its elapsed time and `backtrace`. Duration is elapsed wall time since the fiber resumed, and a report is emitted while the fiber is still executing.

### Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `FIBER_PROFILER_WATCHDOG_STALL_THRESHOLD` | `0.5` | Minimum execution duration in seconds before reporting, and minimum interval between repeated reports. |
| `FIBER_PROFILER_WATCHDOG_SAMPLE_INTERVAL` | `0.1` | Delay in seconds between stack samples. |

Both values must be finite and positive. Actual sampling intervals depend on Ruby and operating-system scheduling. Capture-specific settings, including its sample rate, do not configure watchdog mode.

For explicit instrumentation, use `Fiber::Profiler::Watchdog.new(stall_threshold: 0.5, sample_interval: 0.1, max_samples: 5, output: $stderr)` and the same `start`/`stop` lifecycle as capture. The output must support writes from the watchdog thread. Stop the profiler before closing its output.

### Interpretation and Limits

Repeated frames identify code worth investigating: optimize expensive work, add cooperative yield points, or offload suitable work to a bounded thread pool. Sampling still has overhead; measure it for your workload.

The watchdog requires Ruby thread scheduling. Native code that holds the GVL without allowing other threads to run can prevent sampling entirely. Short stalls can occur between samples, and normal scheduler/OS delays can extend measured execution time. A report is a diagnostic lead, not proof that every sampled frame is expensive.

When profiling starts on a blocking event-loop fiber, that fiber is excluded from monitoring, including its normal idle waits. Other fibers, including blocking application fibers, are monitored. When profiling starts inside a non-blocking application fiber, blocking fibers are excluded because the event-loop fiber is not known. Start and stop profiling on the monitored thread.

## Default Environment Variables

The following settings apply to capture mode only.

### `FIBER_PROFILER_CAPTURE`

Set to `true` to enable capturing of stalled fibers.

### `FIBER_PROFILER_CAPTURE_STALL_THRESHOLD`

Set the threshold in seconds for reporting a stalled fiber. Default is `0.01`.

### `FIBER_PROFILER_CAPTURE_TRACK_CALLS`

Set to `true` to track calls within the fiber. Default is `true`. This can be disabled to reduce overhead.

### `FIBER_PROFILER_CAPTURE_SAMPLE_RATE`

Set the sample rate of the profiler as a percentage of all context switches. The default is 1.0 (100%).

## Analyzing Logs

If you collect your logs in a file (e.g. as `ndjson`) you can analyze them using the included `bake` commands:

```bash
$ bundle exec bake input --file samples.ndjson fiber:profiler:analyze output
```

This will aggregate all the call logs and generate a short summary, ordered by duration.

The analyzer consumes capture-mode call timings. Watchdog reports contain sampled backtraces instead of call timings and are not aggregated by this command.
