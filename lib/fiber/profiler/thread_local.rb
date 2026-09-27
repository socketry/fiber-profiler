# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

require_relative "fork_handler"

module Fiber::Profiler
	# Thread-local storage for the active profiler, shared by both implementations.
	::Thread.attr_accessor :fiber_profiler_capture
	
	::Process.singleton_class.prepend(ForkHandler)
	
	# Manages the active profiler so it can be stopped after a fork.
	module ThreadLocalProfiler
		# Start profiling on the current thread.
		def start
			result = super
			Thread.current.fiber_profiler_capture = self if result
			result
		end
		
		# Stop profiling and clear the current thread's reference.
		def stop
			super
		ensure
			if Thread.current.fiber_profiler_capture.equal?(self)
				Thread.current.fiber_profiler_capture = nil
			end
		end
	end
	
	private_constant :ThreadLocalProfiler
end
