# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "falcon/server"
require "io/endpoint/host_endpoint"
require "io/endpoint/bound_endpoint"
require "async/http/protocol/http1"
require "json"

# Exercise Rack dispatch, Ruby method calls, JSON generation, and cooperative IO waits:
application = lambda do |env|
	if env.fetch("PATH_INFO") == "/io"
		2.times{sleep 0.001}
	end
	
	items = 25.times.map do |index|
		{id: index, title: "Product #{index}", price: (index + 1) * 1.25}
	end
	body = JSON.generate(items: items, total: items.sum{|item| item[:price]})
	[200, {"content-type" => "application/json", "content-length" => body.bytesize.to_s}, [body]]
end

endpoint = IO::Endpoint.tcp("127.0.0.1", 0).bound
server = Falcon::Server.new(Falcon::Server.rack_middleware(application, cache: false), endpoint, protocol: Async::HTTP::Protocol::HTTP1, scheme: "http")
$stdout.sync = true

Sync do
	task = server.run
	profiler = Thread.current.fiber_profiler_capture
	puts JSON.generate(
		port: endpoint.sockets.first.local_address.ip_port,
		ruby: RUBY_DESCRIPTION,
		gems: %w[falcon async io-event rack json].to_h{|name| [name, Gem.loaded_specs.fetch(name).version.to_s]},
		profiler: profiler&.class&.name,
		yjit: defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?
	)
	
	while $stdin.gets
		times = Process.times
		puts JSON.generate(
			cpu: times.utime + times.stime,
			allocations: GC.stat(:total_allocated_objects),
			gc_count: GC.stat(:count),
			stalls: profiler&.stalls || 0,
			error: profiler.respond_to?(:error) ? profiler.error&.message : nil
		)
	end
ensure
	task&.stop
	endpoint&.close
end
