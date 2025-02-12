# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "fiber/profiler/capture"
require "json"

describe Fiber::Profiler::Capture do
	let(:output) {StringIO.new}
	let(:profiler) {subject.new(stall_threshold: 0.0001, output: output)}
	
	with "#stall_threshold" do
		it "should return the stall threshold" do
			expect(profiler).to have_attributes(
				stall_threshold: be == 0.0001
			)
		end
	end
	
	with "#track_calls" do
		it "should return true by default" do
			expect(profiler).to have_attributes(
				track_calls: be == true
			)
		end
	end
	
	it "should start profiling" do
		profiler.start
		
		Fiber.new do
			sleep 0.0001
		end.resume
		
		profiler.stop
		
		expect(profiler).to have_attributes(
			stalls: be >= 1
		)
		
		stall = JSON.parse(output.string)
		expect(stall).to have_keys(
			"duration" => be >= 0.0001,
		)
		
		calls = stall["calls"]
		expect(calls[0]).to have_keys(
			"path" => be == __FILE__,
			"line" => be > 0,
			"class" => be == "Kernel",
			"method" => be == "sleep",
		)
	end
end
