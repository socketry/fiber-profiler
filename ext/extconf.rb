#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "mkmf"

gem_name = File.basename(__dir__)
extension_name = "Fiber_Profiler"

$CFLAGS << " -Wall -Wno-unknown-pragmas -std=c99"

if ENV.key?("RUBY_DEBUG")
	$stderr.puts "Enabling debug mode..."
	
	$CFLAGS << " -DRUBY_DEBUG -O0"
end

$srcs = ["fiber/profiler/profiler.c", "fiber/profiler/time.c", "fiber/profiler/fiber.c", "fiber/profiler/capture.c"]
$VPATH << "$(srcdir)/fiber/profiler"

have_func("rb_fiber_current")

if ENV.key?("RUBY_SANITIZE")
	$stderr.puts "Enabling sanitizers..."
	
	# Add address and undefined behaviour sanitizers:
	$CFLAGS << " -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer"
	$LDFLAGS << " -fsanitize=address -fsanitize=undefined"
end

create_header

# Generate the makefile to compile the native binary into `lib`:
create_makefile(extension_name)
