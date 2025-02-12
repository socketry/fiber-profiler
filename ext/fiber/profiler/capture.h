// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>

extern int Fiber_Profiler_capture;
extern float Fiber_Profiler_Capture_stall_threshold;
extern int Fiber_Profiler_Capture_track_calls;

void Init_Fiber_Profiler_Capture(VALUE Fiber_Profiler);
