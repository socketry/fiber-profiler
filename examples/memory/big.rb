#!/usr/bin/env ruby
# frozen_string_literal: true

require "fiber/profiler"
require "process/metrics"

profiler = Fiber::Profiler.default

profiler&.start

def nested(n = 100, &block)
	if n == 0
		return yield
	end
	
	return nested(n - 1, &block)
end

2.times do
	Fiber.new do
		nested(5) do
			GC.start
		end
		
		nested(10) do
			sleep 0.01
		end
		
		nested(100) do
			# Nothing
		end
	end.resume
end

profiler&.stop

if profiler
	puts "Stalls: #{profiler.stalls}"
	general = Process::Metrics::General.capture(pid: Process.pid)
	puts general
end