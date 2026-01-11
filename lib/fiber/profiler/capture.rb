# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "native"
require_relative "fork_handler"

module Fiber::Profiler
	# Thread-local storage for the active profiler capture.
	::Thread.attr_accessor :fiber_profiler_capture
	
	# Private module that wraps start/stop to manage thread-local storage.
	module ThreadLocalCapture
		# Start profiling and store the capture in the current thread's thread-local storage.
		def start
			result = super
			
			if result
				Thread.current.fiber_profiler_capture = self
			end
			
			return result
		end
		
		# Stop profiling and clear the capture from the current thread's thread-local storage.
		def stop
			result = super
			
			if result
				Thread.current.fiber_profiler_capture = nil
			end
			
			return result
		end
	end
	
	private_constant :ThreadLocalCapture
	
	class Capture
		prepend ThreadLocalCapture
	end
end
