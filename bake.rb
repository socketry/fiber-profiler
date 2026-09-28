# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

def build
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("ruby ./extconf.rb")
		system("make")
	end
end

def clean
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("make clean")
	end
end

# Prepare the project for testing.
#
# @parameter context [Hash] The context of the project.
def before_test
	self.build
end

# Update copyrights and project documentation for the new version.
#
# @parameter version [String] The new version number.
def after_gem_release_version_increment(version)
	context["modernize:license"].call
	context["releases:update"].call(version)
	context["utopia:project:update"].call
end
