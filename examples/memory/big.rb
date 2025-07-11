#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

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

1.times do
	Fiber.new do
		nested(5) do
			GC.start
		end
		
		nested(10) do
			sleep 0.01
		end
		
		nested(2) do
			nested(2) do
				sleep 0.01
			end
			nested(2) do
				sleep 0.01
			end
		end
		
		nested(100) do
			# Nothing
		end
	end.resume
end

def format_memory(size_kb)
	if size_kb < 1024
		return "#{size_kb} KB"
	end
	
	size_mb = size_kb / 1024
	if size_mb < 1024
		return "#{size_mb} MB"
	end
	
	size_gb = size_mb / 1024
	return "#{size_gb} GB"
end

if profiler
	# puts "Stalls: #{profiler.stalls}"
	# memory = Process::Metrics::Memory.capture([Process.pid])
	# puts memory
	# puts format_memory(memory.total_size)
end

profiler&.stop
