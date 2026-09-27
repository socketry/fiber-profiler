# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "fiber/profiler/watchdog"
require "stringio"
require "async"

describe Fiber::Profiler::Watchdog do
	let(:output) {StringIO.new}
	let(:watchdog) {subject.new(stall_threshold: 0.03, sample_interval: 0.005, max_samples: 3, output: output)}
	
	after do
		@watchdog&.stop
	end
	
	def reports
		output.string.lines.map{|line| JSON.parse(line)}
	end
	
	it "samples an ongoing stall and bounds the retained stacks" do
		watchdog.start
		fiber = Fiber.new{sleep 0.15}
		fiber.resume
		watchdog.stop
		
		expect(reports.size).to be >= 1
		reports.each do |report|
			expect(report).to have_keys(
				"mode" => be == "watchdog",
				"pid" => be == Process.pid,
				"thread_id" => be == Thread.current.object_id,
				"fiber_id" => be == fiber.object_id,
				"duration" => be >= 0.03,
			)
			expect(report["samples"].size).to be <= 3
			expect(report["samples"].first["backtrace"].join).to be =~ /sleep/
		end
		expect(watchdog.stalls).to be == reports.size
	end
	
	it "does not report the idle event loop fiber" do
		watchdog.start
		sleep 0.1
		watchdog.stop
		expect(reports).to be == []
	end
	
	it "samples Ruby computation that does not yield" do
		watchdog.start
		Fiber.new do
			deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 0.4
			while Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
			end
		end.resume
		watchdog.stop
		expect(reports.size).to be >= 1
		expect(reports.first["samples"].first["backtrace"].join).to be =~ /watchdog.rb/
	end
	
	def first_stall(notification)
		notification.pop(timeout: 10)
	end
	
	def second_stall(notification)
		notification.pop(timeout: 10)
	end
	
	it "does not mix stacks from different fiber executions" do
		first_report = Queue.new
		second_report = Queue.new
		first = Fiber.new{first_stall(first_report)}
		second = Fiber.new{second_stall(second_report)}
		notifications = {first.object_id => first_report, second.object_id => second_report}
		
		output.define_singleton_method(:write) do |message|
			report = JSON.parse(message)
			notifications.fetch(report["fiber_id"]) << report
			super(message)
		end
		
		# Retain enough samples to expose stacks leaking from the previous execution:
		@watchdog = subject.new(stall_threshold: 0.03, sample_interval: 0.005, max_samples: 100, output: output)
		watchdog.start
		expect(first.resume).not.to be_nil
		expect(second.resume).not.to be_nil
		watchdog.stop
		
		markers = {first.object_id => /first_stall/, second.object_id => /second_stall/}
		expect(reports.map{|report| report["fiber_id"]}.uniq).to be == markers.keys
		reports.each do |report|
			expect(report["samples"].size).to be >= 1
			report["samples"].each do |sample|
				expect(sample["backtrace"].join).to be =~ markers.fetch(report["fiber_id"])
			end
		end
	end
	
	it "monitors blocking application fibers when started on the event loop" do
		watchdog.start
		Fiber.new(blocking: true){sleep 0.1}.resume
		watchdog.stop
		expect(reports.size).to be >= 1
	end
	
	it "can start inside a non-blocking fiber" do
		Fiber.new do
			watchdog.start
			sleep 0.1
			watchdog.stop
		end.resume
		expect(reports.size).to be >= 1
	end
	
	it "can stop and restart without leaking sampling threads" do
		threads = Thread.list
		2.times do
			expect(watchdog.start).to be == watchdog
			expect(watchdog.start).to be == false
			expect(Thread.current.fiber_profiler_capture).to be == watchdog
			expect(watchdog.stop).to be == watchdog
			expect(watchdog.stop).to be == false
			expect(Thread.current.fiber_profiler_capture).to be_nil
		end
		expect(Thread.list).to be == threads
	end
	
	it "removes instrumentation if starting fails" do
		threads = Thread.list
		def watchdog.record_execution
			raise "Cannot record execution"
		end
		
		expect{watchdog.start}.to raise_exception(RuntimeError, message: be == "Cannot record execution")
		expect(watchdog.stop).to be == false
		expect(Thread.current.fiber_profiler_capture).to be_nil
		expect(Thread.list).to be == threads
		
		# A leftover tracepoint would raise again on this switch:
		expect(Fiber.new{:switched}.resume).to be == :switched
	end
	
	it "only monitors its owning thread" do
		watchdog.start
		Thread.new do
			Fiber.new{sleep 0.1}.resume
		end.join
		watchdog.stop
		expect(reports).to be == []
	end
	
	it "supports a scheduler on a background thread" do
		Thread.new do
			scheduler = Async::Scheduler.new(profiler: watchdog)
			Fiber.set_scheduler(scheduler)
			scheduler.run{Fiber.blocking{sleep 0.1}}
		ensure
			Fiber.set_scheduler(nil)
		end.value
		expect(reports.size).to be >= 1
		expect(reports.first["thread_id"]).not.to be == Thread.current.object_id
	end
	
	it "does not report scheduler-aware sleep" do
		scheduler = Async::Scheduler.new(profiler: watchdog)
		Fiber.set_scheduler(scheduler)
		scheduler.run{sleep 0.1}
		expect(reports).to be == []
	ensure
		Fiber.set_scheduler(nil)
	end
	
	it "cleans up when the monitored workload fails" do
		scheduler = Async::Scheduler.new(profiler: watchdog)
		Fiber.set_scheduler(scheduler)
		threads = Thread.list
		task = scheduler.run(finished: false){raise "Failed workload"}
		expect{task.wait}.to raise_exception(RuntimeError, message: be == "Failed workload")
		expect(Thread.list).to be == threads
		expect(Thread.current.fiber_profiler_capture).to be_nil
	ensure
		Fiber.set_scheduler(nil)
	end
	
	it "disables inherited monitoring after fork and can restart in the child" do
		watchdog.start
		pid = fork do
			exit(1) unless Thread.current.fiber_profiler_capture.nil?
			exit(2) unless watchdog.stop == false
			watchdog.start
			Fiber.new{sleep 0.1}.resume
			watchdog.stop
			exit(watchdog.stalls > 0 ? 0 : 3)
		end
		_, status = Process.wait2(pid)
		expect(status.exitstatus).to be == 0
		expect(Thread.current.fiber_profiler_capture).to be == watchdog
		Fiber.new{sleep 0.1}.resume
		watchdog.stop
		expect(reports.size).to be >= 1
	end
	
	it "reads watchdog-specific configuration" do
		instance = subject.default("FIBER_PROFILER_WATCHDOG_STALL_THRESHOLD" => "2", "FIBER_PROFILER_WATCHDOG_SAMPLE_INTERVAL" => "0.2")
		expect(instance.stall_threshold).to be == 2.0
		expect(instance.sample_interval).to be == 0.2
	end
	
	it "rejects invalid intervals and sample limits" do
		[0, -1, Float::INFINITY, Float::NAN].each do |value|
			expect{subject.new(stall_threshold: value)}.to raise_exception(ArgumentError)
			expect{subject.new(sample_interval: value)}.to raise_exception(ArgumentError)
		end
		[0, -1, 1.5].each do |value|
			expect{subject.new(max_samples: value)}.to raise_exception(ArgumentError)
		end
	end
	
	it "formats terminal output as readable stacks" do
		def output.tty?
			true
		end
		watchdog.start
		Fiber.new{sleep 0.1}.resume
		watchdog.stop
		expect(output.string).to be =~ /Fiber stalled for at least/
		expect(output.string).to be =~ /sleep/
	end
end
