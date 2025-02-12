# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "profiler/version"
require_relative "profiler/native"

module Fiber::Profiler
	def self.default
		Capture.default
	end
end
