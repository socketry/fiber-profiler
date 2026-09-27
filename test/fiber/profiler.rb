# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "fiber/profiler"
require "open3"
require "rbconfig"
require "json"

describe Fiber::Profiler do
	def default_profiler(environment)
		environment = {"FIBER_PROFILER" => nil, "FIBER_PROFILER_CAPTURE" => nil}.merge(environment)
		code = 'require "fiber/profiler"; puts Fiber::Profiler.default.class.name'
		Open3.capture3(environment, RbConfig.ruby, "-Ilib", "-Iext", "-e", code)
	end
	
	{
		{} => "NilClass",
		{"FIBER_PROFILER_CAPTURE" => "true"} => "Fiber::Profiler::Capture",
		{"FIBER_PROFILER_CAPTURE" => "watchdog"} => "NilClass",
		{"FIBER_PROFILER" => "capture"} => "Fiber::Profiler::Capture",
		{"FIBER_PROFILER" => "watchdog"} => "Fiber::Profiler::Watchdog",
		{"FIBER_PROFILER" => "watchdog", "FIBER_PROFILER_CAPTURE" => "true"} => "Fiber::Profiler::Watchdog",
		{"FIBER_PROFILER" => "capture", "FIBER_PROFILER_CAPTURE" => "false"} => "Fiber::Profiler::Capture",
		{"FIBER_PROFILER" => "false", "FIBER_PROFILER_CAPTURE" => "true"} => "NilClass",
	}.each do |environment, name|
		it "selects #{name} with #{environment.inspect}" do
			output, errors, status = default_profiler(environment)
			expect(status.success?).to be == true
			expect(output.strip).to be == name
			expect(errors).to be == ""
		end
	end
	
	it "rejects an unknown mode" do
		output, errors, status = default_profiler("FIBER_PROFILER" => "watcdhog")
		expect(status.success?).to be == false
		expect(errors).to be =~ /Unknown FIBER_PROFILER mode/
	end
	
	it "retains capture-specific configuration in explicit capture mode" do
		output, errors, status = Open3.capture3(
			{"FIBER_PROFILER" => "capture", "FIBER_PROFILER_CAPTURE" => nil, "FIBER_PROFILER_CAPTURE_TRACK_CALLS" => "false", "FIBER_PROFILER_CAPTURE_STALL_THRESHOLD" => "0.25"},
			RbConfig.ruby, "-Ilib", "-Iext", "-e",
			'require "fiber/profiler"; capture = Fiber::Profiler.default; puts [capture.track_calls, capture.stall_threshold].inspect'
		)
		expect(status.success?).to be == true
		expect(output.strip).to be == "[false, 0.25]"
	end
	
	it "automatically starts and stops watchdog mode with Async" do
		output, errors, status = Open3.capture3(
			{"FIBER_PROFILER" => "watchdog", "FIBER_PROFILER_WATCHDOG_STALL_THRESHOLD" => "0.03", "FIBER_PROFILER_WATCHDOG_SAMPLE_INTERVAL" => "0.005"},
			RbConfig.ruby, "-Ilib", "-Iext", "-e",
			'require "async"; threads = Thread.list; Sync { Fiber.blocking { sleep 0.1 } }; abort "Leaked watchdog" unless Thread.list == threads'
		)
		expect(status.success?).to be == true
		reports = errors.lines.map{|line| JSON.parse(line)}
		expect(reports.size).to be >= 1
		expect(reports.first["mode"]).to be == "watchdog"
	end
	
	["false", "capture", "watchdog"].each do |mode|
		it "cleans up the capture block helper in #{mode} mode" do
			output, errors, status = Open3.capture3(
				{"FIBER_PROFILER" => mode}, RbConfig.ruby, "-Ilib", "-Iext", "-e", <<~RUBY
					require "fiber/profiler"
					threads = Thread.list
					begin
						Fiber::Profiler.capture { raise "Expected failure" }
					rescue RuntimeError => error
						abort error.message unless error.message == "Expected failure"
					end
					abort "Leaked profiler" if Thread.current.fiber_profiler_capture
					abort "Leaked thread" unless Thread.list == threads
				RUBY
			)
			expect(status.success?).to be == true
			expect(errors).to be == ""
		end
	end
end
