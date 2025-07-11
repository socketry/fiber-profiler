# frozen_string_literal: true

require_relative "lib/fiber/profiler/version"

Gem::Specification.new do |spec|
	spec.name = "fiber-profiler"
	spec.version = Fiber::Profiler::VERSION
	
	spec.summary = "A fiber stall profiler."
	spec.authors = ["Samuel Williams"]
	spec.license = "MIT"
	
	spec.cert_chain  = ["release.cert"]
	spec.signing_key = File.expand_path("~/.gem/release.pem")
	
	spec.homepage = "https://github.com/socketry/fiber-profiler"
	
	spec.metadata = {
		"documentation_uri" => "https://socketry.github.io/fiber-profiler/",
		"source_code_uri" => "https://github.com/socketry/fiber-profiler.git",
	}
	
	spec.files = Dir["{ext,lib}/**/*", "*.md", base: __dir__]
	spec.require_paths = ["lib"]
	
	spec.extensions = ["ext/extconf.rb"]
	
	spec.required_ruby_version = ">= 3.2"
end
