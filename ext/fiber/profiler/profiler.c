// Released under the MIT License.
// Copyright, 2021-2025, by Samuel Williams.

#include "profiler.h"

#include "fiber.h"
#include "capture.h"

void Init_Fiber_Profiler(void)
{
#ifdef HAVE_RB_EXT_RACTOR_SAFE
	rb_ext_ractor_safe(true);
#endif
	
	VALUE Fiber = rb_const_get(rb_cObject, rb_intern("Fiber"));
	VALUE Fiber_Profiler = rb_define_module_under(Fiber, "Profiler");
	
	Init_Fiber_Profiler_Fiber(Fiber_Profiler);
	Init_Fiber_Profiler_Capture(Fiber_Profiler);
}
