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
	
	with "Process.fork" do
		it "should disable the profiler in the child process after fork" do
			capture.start
			
			# Verify profiler is running in parent - thread-local should be set
			expect(Thread.current.fiber_profiler_capture).to be == capture
			
			# Verify start returns false when already running
			expect(capture.start).to be == false
			
			# Fork and verify behavior in child
			pid = fork do
				# In child process: profiler should be stopped by fork handler
				exit(1) unless Thread.current.fiber_profiler_capture.nil?
				
				# Verify stop returns false when not running
				exit(2) unless capture.stop == false
				
				# Verify we can start again in child if needed
				exit(3) unless capture.start
				exit(4) unless Thread.current.fiber_profiler_capture == capture
				
				# Exit child process successfully
				exit(0)
			end
			
			# Wait for child to complete and check exit status
			_, status = Process.wait2(pid)
			expect(status.exitstatus).to be == 0
			
			# In parent process: profiler should still be running
			expect(Thread.current.fiber_profiler_capture).to be == capture
			
			# Verify profiling still works in parent
			Fiber.new do
				sleep 0.001
			end.resume
			
			capture.stop
			
			# After stop: profiler should be stopped and thread-local cleared
			expect(Thread.current.fiber_profiler_capture).to be_nil
			expect(capture.stop).to be == false
		end
	end
end
