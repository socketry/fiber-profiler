# Getting Started

This guide explains how to detect stalls using the fiber profiler.

## Installation

Add the gem to your project:

```bash
$ bundle add fiber-profiler
```

## Usage

Instrument your code using the default profiler:

```ruby
#!/usr/bin/env ruby

require 'fiber/profiler'

profiler = Fiber::Profiler.default

profiler&.start

# Your application code:
Fiber.new do
	sleep 0.1
end.resume

profiler&.stop
```

Running this program will output the following:

```bash
$ FIBER_PROFILER_CAPTURE=true bundle exec ./test.rb
Fiber stalled for 0.105 seconds
/Users/samuel/Developer/socketry/fiber-profiler/test.rb:11 in c-call 'Kernel#sleep' (0.105s)
```

## Integration with Async

The fiber profiler is optionally supported by `Async`. Simply enable the profiler using `FIBER_PROFILER_CAPTURE=true` to capture and report stalls.

## Default Environment Variables

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
