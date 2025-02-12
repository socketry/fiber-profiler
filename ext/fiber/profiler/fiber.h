// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>

#ifdef HAVE_RB_FIBER_CURRENT
#define Fiber_Profiler_Fiber_current() rb_fiber_current()
#else
VALUE Fiber_Profiler_Fiber_current(void);
#endif

int Fiber_Profiler_Fiber_blocking(VALUE fiber);

void Init_Fiber_Profiler_Fiber(VALUE Fiber_Profiler);
