# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

require "fiber/profiler/capture"
require "json"

describe Fiber::Profiler::Capture do
	let(:output) {StringIO.new}
	let(:capture) {subject.new(stall_threshold: 0.0001, output: output)}
	
	after do
		@capture&.stop
	end
	
	with "#stall_threshold" do
		it "should return the stall threshold" do
			expect(capture).to have_attributes(
				stall_threshold: be == 0.0001
			)
		end
	end
	
	with "#track_calls" do
		it "should return true by default" do
			expect(capture).to have_attributes(
				track_calls: be == true
			)
		end
	end
	
	with "#sample_rate" do
		let(:capture) {subject.new(stall_threshold: 0.0001, sample_rate: 0.1, output: output)}
		
		it "should return the sample rate" do
			expect(capture).to have_attributes(
				sample_rate: be == 0.1
			)
		end
		
		it "should sample at the given rate" do
			capture.start
			
			100.times do
				Fiber.new do
					sleep 0.0001
				end.resume
			end
			
			capture.stop
			
			expect(capture).to have_attributes(
				stalls: (be >= 1).and(be <= 50)
			)
		end
	end
	
	with "#start" do
		it "should start profiling" do
			capture.start
			
			Fiber.new do
				sleep 0.001
			end.resume
			
			capture.stop
			
			expect(capture).to have_attributes(
				stalls: be >= 1
			)
			
			stall = JSON.parse(output.string)
			expect(stall).to have_keys(
				"duration" => be >= 0.0001,
			)
			
			calls = stall["calls"]
			expect(calls).to have_value(have_keys(
				"path" => be == __FILE__,
				"line" => be > 0,
				"class" => be == "Kernel",
				"method" => be == "sleep",
			))
		end
		
		def nested(n = 100, &block)
			if n == 0
				return yield
			end
			
			return nested(n - 1, &block)
		end
		
		it "can profile more complex call stacks" do
			capture.start
			
			2.times do
				Fiber.new do
					nested(1000) do
						# Nothing
					end
					
					nested(1000) do
						sleep 0.01
					end
					
					nested(1000) do
						# Nothing
					end
				end.resume
			end
			
			capture.stop
			
			expect(capture).to have_attributes(
				stalls: be == 2
			)
		end
		
		it "can detect garbage collection stalls" do
			capture.start
			
			Fiber.new do
				GC.start
				sleep(0.001)
			end.resume
			
			capture.stop
			
			expect(capture).to have_attributes(
				stalls: be == 1
			)
			
			stall = JSON.parse(output.string)
			calls = stall["calls"]
			expect(calls).to have_value(have_keys(
				"path" => be =~ /internal:gc/,
			))
		end
	end
end
