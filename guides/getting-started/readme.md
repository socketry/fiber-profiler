# Getting Started

This guide explains how to install the fiber profiler and choose a mode for diagnosing event-loop stalls.

## Installation

Add the gem to your project:

```bash
$ bundle add fiber-profiler
```

## Choose a Mode

A fiber that runs for too long without yielding prevents other work on the same event loop from progressing. The profiler offers two ways to investigate it:

| Mode | Use it to | Reports |
| --- | --- | --- |
| [Watchdog](../watchdog-mode/index) | Find where a fiber is spending time during an ongoing stall. | Periodically sampled backtraces while the fiber is still executing. |
| [Capture](../capture-mode/index) | Investigate individual call timings during a fiber's execution. | Traced calls and their durations after the fiber switches away. |

Watchdog avoids method-call tracing and is a useful starting point for observing sustained stalls. Capture provides more detail at a higher instrumentation cost. Both affect the application being measured; see the [Falcon overhead benchmark](https://github.com/socketry/fiber-profiler/tree/main/benchmark/falcon) and measure with your own workload.

## Integration with Async and Falcon

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

Each scheduler obtains its own profiler, so schedulers in separate worker processes or threads are monitored independently. After `fork`, inherited profiling is stopped in the child; a new scheduler starts a new profiler normally.

## Next Steps

- [Watchdog Mode](../watchdog-mode/index): reproduce a stall, configure stack sampling, and interpret ongoing reports.
- [Capture Mode](../capture-mode/index): trace calls, control sampling overhead, and aggregate timing logs.

Both guides include manual `start`/`stop` examples for code outside Async. Use those examples when your application manages fibers directly.
