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
	
	with "#sample_rate" do
		let(:profiler) {subject.new(stall_threshold: 0.0001, sample_rate: 0.1, output: output)}
		
		it "should return the sample rate" do
			expect(profiler).to have_attributes(
				sample_rate: be == 0.1
			)
		end
		
		it "should sample at the given rate" do
			profiler.start
			
			100.times do
				Fiber.new do
					sleep 0.0001
				end.resume
			end
			
			profiler.stop
			
			expect(profiler).to have_attributes(
				stalls: (be >= 1).and(be <= 50)
			)
		end
	end
	
	with "#start" do
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
		
		def nested(n = 100, &block)
			if n == 0
				return yield
			end
			
			return nested(n - 1, &block)
		end
		
		it "can profile more complex call stacks" do
			profiler.start
			
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
			
			profiler.stop
			
			expect(profiler).to have_attributes(
				stalls: be == 2
			)
		end
		
		it "can detect garbage collection stalls" do
			profiler.start
			
			Fiber.new do
				GC.start
				sleep(0.0001)
			end.resume
			
			profiler.stop
			
			expect(profiler).to have_attributes(
				stalls: be == 1
			)
			
			stall = JSON.parse(output.string)
			calls = stall["calls"]
			expect(calls[0]).to have_keys(
				"path" => be =~ /internal:gc/,
			)
		end
	end
end
