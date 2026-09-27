# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

require_relative "native"
require_relative "thread_local"

module Fiber::Profiler
	# Represents a running profiler capture.
	class Capture
		prepend ThreadLocalProfiler
	end
end
