# Watchdog Mode

This guide explains how to sample ongoing fiber stalls with the watchdog profiler.

Use watchdog mode to find where a fiber is spending time while it prevents other fibers from running. It reports during the stall, so you can investigate an operation that has not returned yet. For individual call timings, see [Capture Mode](../capture-mode/index).

## Usage

Add `fiber-profiler` to your application's bundle as described in [Getting Started](../getting-started/index), then select watchdog mode when starting Async or Falcon:

```bash
FIBER_PROFILER=watchdog bundle exec falcon serve
```

Async starts and stops the watchdog automatically. Each scheduler gets its own profiler, which binds to the thread when profiling starts.

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

## How Sampling Works

Watchdog uses a thread-specific `:fiber_switch` tracepoint to track which fiber is executing. A separate Ruby thread periodically captures the monitored thread's backtrace. It does not install method-call tracing hooks or require a heartbeat task.

By default, it samples every 100 ms, retains at most five recent stacks, and reports when the same fiber execution lasts at least 500 ms. A continued stall produces at most one report per additional threshold interval. Returning to the event loop or switching to another fiber resets the sample window. The measured duration covers one uninterrupted fiber execution.

## Configuration

Set these environment variables before starting your application. They are read when the default watchdog is constructed:

| Variable | Default | Meaning |
| --- | --- | --- |
| `FIBER_PROFILER_WATCHDOG_STALL_THRESHOLD` | `0.5` | Minimum execution duration in seconds before reporting, and minimum interval between repeated reports. |
| `FIBER_PROFILER_WATCHDOG_SAMPLE_INTERVAL` | `0.1` | Delay in seconds between stack samples. |

Both values must be finite and positive. Actual sampling intervals depend on Ruby and operating-system scheduling. Capture-specific settings, including its sample rate, do not configure watchdog mode.

### Manual Instrumentation

When managing fibers directly, construct a {ruby Fiber::Profiler::Watchdog} and stop it in an `ensure` block:

```ruby
require "fiber/profiler/watchdog"

watchdog = Fiber::Profiler::Watchdog.new(
	stall_threshold: 0.5,
	sample_interval: 0.1,
	max_samples: 5,
	output: $stderr
)

begin
	watchdog.start
	
	# Simulate an application fiber blocked on an operation:
	Fiber.new do
		sleep 1
	end.resume
ensure
	watchdog.stop
end
```

This example starts the profiler explicitly, so it does not need `FIBER_PROFILER=watchdog`. Start and stop profiling on the monitored thread. `max_samples` must be a positive integer; there is no environment variable for it. The output must support writes from the watchdog thread. Stop the profiler before closing its output.

When profiling starts on a blocking event-loop fiber, that fiber is excluded from monitoring, including its normal idle waits. Other fibers, including blocking application fibers, are monitored. When profiling starts inside a non-blocking application fiber, blocking fibers are excluded because the event-loop fiber is not known.

If sampling or reporting raises a `StandardError`, the watchdog disables its tracepoint and stops sampling. It retains the exception in `watchdog.error` and attempts one warning to standard error. Failure to write that warning is also contained. These errors do not escape through `stop` or replace application errors. Call `stop` normally to finish cleanup; a subsequent `start` clears the error and resumes monitoring.

## Reading Reports

Terminal output contains readable stacks. Redirected output uses one JSON object per report:

```bash
FIBER_PROFILER=watchdog bundle exec ruby stall.rb 2> watchdog.ndjson
```

Each report contains `mode: "watchdog"`, process/thread/fiber identifiers, elapsed `duration`, and a bounded `samples` array. Each sample contains its elapsed time and `backtrace`. Duration is elapsed wall time since the fiber resumed, and a report is emitted while the fiber is still executing.

Repeated frames identify code worth investigating: optimize expensive work, add cooperative yield points, or offload suitable work to a bounded thread pool. Watchdog reports contain sampled backtraces, so the capture-mode `fiber:profiler:analyze` command does not aggregate them.

## Interpretation and Limits

The watchdog requires Ruby thread scheduling. Native code that holds the GVL without allowing other threads to run can prevent sampling entirely. Short stalls can occur between samples, and normal scheduler/OS delays can extend measured execution time. A report is a diagnostic lead, not proof that every sampled frame is expensive.

Sampling still has overhead. Use the [Falcon overhead benchmark](https://github.com/socketry/fiber-profiler/tree/main/benchmark/falcon) as a starting point for measuring your own application before enabling continuous monitoring.
