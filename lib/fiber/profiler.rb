# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "profiler/version"
require_relative "profiler/native"

module Fiber::Profiler
	# The default profiler to use, if any.
	#
	# Use the `FIBER_PROFILER_CAPTURE=true` environment variable to enable profiling.
	#
	# @returns [Capture | Nil]
	def self.default
		Capture.default
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
