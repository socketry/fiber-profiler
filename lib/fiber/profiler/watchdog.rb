# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require_relative "version"
require_relative "thread_local"
require "json"

module Fiber::Profiler
	# Samples stacks while a fiber runs without switching back to the event loop.
	class Watchdog
		prepend ThreadLocalProfiler
		
		# Build a watchdog using environment configuration.
		# @parameter env [Hash] The environment to read configuration from.
		# @returns [Watchdog] The configured watchdog.
		def self.default(env = ENV)
			new(
				stall_threshold: Float(env.fetch("FIBER_PROFILER_WATCHDOG_STALL_THRESHOLD", "0.5")),
				sample_interval: Float(env.fetch("FIBER_PROFILER_WATCHDOG_SAMPLE_INTERVAL", "0.1"))
			)
		end
		
		# Initialize a watchdog. Monitoring begins when {start} is called.
		# @parameter stall_threshold [Float] Seconds without a fiber switch before reporting, and the minimum interval between reports for the same execution.
		# @parameter sample_interval [Float] Seconds between stack samples.
		# @parameter max_samples [Integer] Maximum number of recent stacks retained per execution.
		# @parameter output [IO] The destination for stall reports.
		def initialize(stall_threshold: 0.5, sample_interval: 0.1, max_samples: 5, output: $stderr)
			@stall_threshold = Float(stall_threshold)
			@sample_interval = Float(sample_interval)
			
			unless @stall_threshold.finite? && @stall_threshold.positive? && @sample_interval.finite? && @sample_interval.positive?
				raise ArgumentError, "Watchdog intervals must be finite and positive"
			end
			unless max_samples.is_a?(Integer) && max_samples.positive?
				raise ArgumentError, "max_samples must be a positive integer"
			end
			
			@max_samples = max_samples
			@output = output
			@running = false
			@stalls = 0
		end
		
		# @attribute [Float] The minimum execution duration before reporting a stall.
		attr_reader :stall_threshold
		
		# @attribute [Float] The delay between samples.
		attr_reader :sample_interval
		
		# @attribute [Integer] The number of reports written.
		attr_reader :stalls
		
		# Start monitoring application fibers on the calling thread.
		# @returns [Watchdog | false] Self, or false if already running.
		def start
			return false if @running
			
			@thread = Thread.current
			@pid = Process.pid
			@mutex = Mutex.new
			@condition = ConditionVariable.new
			@running = true
			@execution = nil
			
			# When started on the event loop, distinguish it from blocking application fibers:
			@loop = Fiber.current if Fiber.current.blocking?
			@tracepoint = TracePoint.new(:fiber_switch){record_execution}
			@tracepoint.enable(target_thread: @thread)
			record_execution
			
			@watchdog = Thread.new{watch}
			self
		rescue Exception
			stop
			raise
		end
		
		# Stop monitoring and wait for the sampling thread to finish.
		# @returns [Watchdog | false] Self, or false if already stopped.
		def stop
			return false unless @running
			
			@tracepoint&.disable
			
			if @pid == Process.pid
				@mutex.synchronize do
					@running = false
					@condition.broadcast
				end
				@watchdog&.join
			else
				# The sampling thread does not survive fork; avoid inherited synchronization:
				@running = false
			end
			
			self
		ensure
			@watchdog = @thread = @execution = @loop = @tracepoint = nil
		end
		
		private
		
		def now
			Process.clock_gettime(Process::CLOCK_MONOTONIC)
		end
		
		def record_execution
			fiber = Fiber.current
			idle = @loop ? fiber.equal?(@loop) : fiber.blocking?
			
			@mutex.synchronize do
				@execution = idle ? nil : [fiber, now]
			end
		end
		
		def wait
			@mutex.synchronize do
				@condition.wait(@mutex, @sample_interval) if @running
				@running
			end
		end
		
		def watch
			execution = nil
			samples = []
			next_report = @stall_threshold
			
			while wait
				current = @mutex.synchronize{@execution}
				unless current.equal?(execution)
					execution = current
					samples.clear
					next_report = @stall_threshold
				end
				next unless execution
				
				backtrace = @thread.backtrace
				duration = now - execution[1]
				
				# Discard samples if the target switched fibers while capturing its stack:
				next unless backtrace && @mutex.synchronize{@execution.equal?(execution)}
				
				samples << {elapsed: duration, backtrace: backtrace}
				samples.shift if samples.size > @max_samples
				
				if duration >= next_report
					report(execution[0], duration, samples)
					@stalls += 1
					next_report = duration + @stall_threshold
				end
			end
		end
		
		def report(fiber, duration, samples)
			if @output.respond_to?(:tty?) && @output.tty?
				message = "## Fiber stalled for at least %.3f seconds (watchdog, pid=%d, thread=%d, fiber=%d)\n" % [duration, @pid, @thread.object_id, fiber.object_id]
				message << samples.map{|sample| sample[:backtrace].join("\n")}.join("\n\n") << "\n"
			else
				message = JSON.generate(mode: "watchdog", pid: @pid, thread_id: @thread.object_id, fiber_id: fiber.object_id, duration: duration, samples: samples) << "\n"
			end
			
			@output.write(message)
		end
	end
end
