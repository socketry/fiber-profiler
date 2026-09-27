# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

require_relative "profiler/version"
require_relative "profiler/capture"

module Fiber::Profiler
	# The default profiler to use, if any.
	#
	# Set `FIBER_PROFILER` to `watchdog`, `capture`, or `false`. When unset,
	# the legacy `FIBER_PROFILER_CAPTURE=true` setting enables capture mode.
	#
	# @returns [Capture | Watchdog | Nil]
	# @raises [ArgumentError] If the requested mode is unknown.
	def self.default
		case mode = ENV["FIBER_PROFILER"]
		when nil
			Capture.default
		when "capture"
			Capture.new
		when "watchdog"
			require_relative "profiler/watchdog"
			Watchdog.default
		when "false"
			nil
		else
			raise ArgumentError, "Unknown FIBER_PROFILER mode: #{mode.inspect} (expected watchdog, capture, or false)"
		end
	end
	
	# Execute the given block with the {default} profiler, if any.
	#
	# @yields {...} The block to execute.
	def self.capture
		if capture = self.default
			begin
				capture.start
				
				yield
			ensure
				capture.stop
			end
		else
			yield
		end
	end
end
