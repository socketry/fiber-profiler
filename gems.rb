# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.

source "https://rubygems.org"

gemspec

gem "process-metrics"

group :maintenance, optional: true do
	gem "bake-gem-github", ">= 0.5.0"
	gem "agent-context"
	gem "bake-modernize"
	gem "bake-releases"
	
	gem "decode"
	
	gem "utopia-project"
end

group :test do
	gem "sus"
	gem "covered"
	
	gem "rubocop"
	gem "rubocop-md"
	gem "rubocop-socketry"
	
	gem "bake-test"
	gem "bake-test-external"
	gem "async"
end
