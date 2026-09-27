# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "json"
require "open3"
require "rbconfig"
require "tempfile"
require "timeout"

def read_snapshot(input, output)
	input.puts("snapshot")
	input.flush
	Timeout.timeout(15){JSON.parse(output.readline)}
end

def load_server(ab, url, duration, concurrency)
	output, status = Open3.capture2e(ab, "-k", "-q", "-t", duration.to_s, "-n", "1000000000", "-c", concurrency.to_s, url)
	raise output unless status.success?
	raise output unless output.match?(/^Failed requests:\s+0$/) && !output.match?(/^Non-2xx responses:\s+[1-9]/)
	output
end

root = File.expand_path("../..", __dir__)
rounds = Integer(ENV.fetch("ROUNDS", "5"))
duration = Integer(ENV.fetch("DURATION", "5"))
warmup = Integer(ENV.fetch("WARMUP", "2"))
concurrency = Integer(ENV.fetch("CONCURRENCY", "32"))
ab = ENV.fetch("AB", "ab")
raise "Benchmark parameters must be positive" unless [rounds, duration, warmup, concurrency].all?(&:positive?)

environment = ENV.keys.grep(/\AFIBER_PROFILER/).to_h{|name| [name, nil]}
$stdout.sync = true

%w[json io].each do |workload|
	rounds.times do |round|
		# Rotate mode order to reduce systematic warm-up and thermal bias:
		%w[false watchdog capture].rotate(round).each do |mode|
			Tempfile.create("fiber-profiler-benchmark") do |log|
				Open3.popen2(environment.merge("FIBER_PROFILER" => mode), RbConfig.ruby, "-I#{root}/lib", "-I#{root}/ext", "#{__dir__}/server.rb", err: log) do |input, output, process|
					begin
						metadata = Timeout.timeout(15){JSON.parse(output.readline)}
						expected = {"false" => nil, "watchdog" => "Fiber::Profiler::Watchdog", "capture" => "Fiber::Profiler::Capture"}.fetch(mode)
						raise "Unexpected profiler: #{metadata["profiler"].inspect}" unless metadata.fetch("profiler") == expected
						url = "http://127.0.0.1:#{metadata.fetch("port")}/#{workload}"
						load_server(ab, url, warmup, concurrency)
						before = read_snapshot(input, output)
						result = load_server(ab, url, duration, concurrency)
						after = read_snapshot(input, output)
						raise "Watchdog failed: #{after["error"]}" if after["error"]
						requests = Integer(result.match(/^Complete requests:\s+(\d+)/)[1])
						puts JSON.generate(
							workload: workload, mode: mode, round: round + 1, concurrency: concurrency, duration: duration, warmup: warmup,
							requests: requests,
							requests_per_second: Float(result.match(/^Requests per second:\s+([\d.]+)/)[1]),
							mean_ms: Float(result.match(/^Time per request:\s+([\d.]+)\s+\[ms\] \(mean\)/)[1]),
							p95_ms: Integer(result.match(/^\s+95%\s+(\d+)/)[1]),
							p99_ms: Integer(result.match(/^\s+99%\s+(\d+)/)[1]),
							cpu_us_per_request: (after.fetch("cpu") - before.fetch("cpu")) * 1_000_000 / requests,
							allocations_per_request: (after.fetch("allocations") - before.fetch("allocations")).fdiv(requests),
							gc_count: after.fetch("gc_count") - before.fetch("gc_count"),
							stalls: after.fetch("stalls") - before.fetch("stalls"),
							log_bytes: log.size,
							server: metadata
						)
					ensure
						input.close
						unless process.join(5)
							Process.kill("KILL", process.pid)
							process.join
						end
						unless process.value.success?
							warn File.read(log.path)
							raise "Falcon benchmark server failed"
						end
					end
				end
			end
		end
	end
end
