# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

module Fiber::Profiler
	# Private module that hooks into Process._fork to handle fork events.
	#
	# If `Scheduler#process_fork` hook is adopted in Ruby 4, this code can be removed after Ruby < 4 is no longer supported.
	module ForkHandler
		def _fork(&block)
			result = super
			
			if result.zero?
				# Child process: disable the profiler for the current thread
				if capture = Thread.current.fiber_profiler_capture
					begin
						capture.stop
					rescue
						# Ignore errors - the profiler may be in an invalid state after fork
					end
				end
			end
			
			return result
		end
	end
	
	private_constant :ForkHandler
	
	# Hook into Process._fork to handle fork events automatically:
	::Process.singleton_class.prepend(ForkHandler)
end
