// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "fiber.h"

#ifndef HAVE_RB_FIBER_CURRENT
static ID id_current;

static VALUE Fiber_Profiler_Fiber_current(void) {
	return rb_funcall(rb_cFiber, id_current, 0);
}
#endif

// There is no public interface for this... yet.
static ID id_blocking_p;

int Fiber_Profiler_Fiber_blocking(VALUE fiber) {
	return RTEST(rb_funcall(fiber, id_blocking_p, 0));
}

void Init_Fiber_Profiler_Fiber(VALUE Fiber_Profiler) {
#ifndef HAVE_RB_FIBER_CURRENT
	id_current = rb_intern("current");
#endif
	
	id_blocking_p = rb_intern("blocking?");
}
