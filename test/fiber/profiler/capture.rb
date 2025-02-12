# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "fiber/profiler/capture"

describe Fiber::Profiler::Capture do
	let(:profiler) {subject.new(0.0001)}

	it "should start profiling" do
		profiler.start
		
		Fiber.new do
			sleep 0.0001
		end.resume
		
		profiler.stop
		
		expect(profiler).to have_attributes(
			stalls: be >= 1
		)
	end
end
