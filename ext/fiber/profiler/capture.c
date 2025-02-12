// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"

#include "time.h"
#include "fiber.h"
#include "array.h"

#include <ruby/debug.h>
#include <stdio.h>

VALUE Fiber_Profiler_Capture = Qnil;

struct Fiber_Profiler_Capture_Call {
	struct timespec enter_time;
	struct timespec exit_time;
	
	size_t nesting;
	
	rb_event_flag_t event_flag;
	ID id;
	
	VALUE klass;
	const char *path;
	int line;
	
	struct Fiber_Profiler_Capture_Call *parent;
};

struct Fiber_Profiler_Capture {
	// Configuration:
	float log_threshold;
	int track_calls;
	
	// Whether or not the profiler is currently running:
	int running;
	
	// Whether or not to capture call data:
	int capture;
	
	size_t stalls;
	
	// From this point on, the state of any profile in progress:
	struct timespec start_time;
	struct timespec stop_time;
	
	// The depth of the call stack:
	size_t nesting;
	
	// The current call frame:
	struct Fiber_Profiler_Capture_Call *current;
	
	struct Fiber_Profiler_Array calls;
};

void Fiber_Profiler_Capture_reset(struct Fiber_Profiler_Capture *profiler) {
	profiler->nesting = 0;
	profiler->current = NULL;
	Fiber_Profiler_Array_truncate(&profiler->calls, 0);
}

void Fiber_Profiler_Capture_Call_initialize(struct Fiber_Profiler_Capture_Call *call) {
	call->enter_time.tv_sec = 0;
	call->enter_time.tv_nsec = 0;
	call->exit_time.tv_sec = 0;
	call->exit_time.tv_nsec = 0;
	
	call->nesting = 0;
	
	call->event_flag = 0;
	call->id = 0;
	
	call->path = NULL;
	call->line = 0;
}

void Fiber_Profiler_Capture_Call_free(struct Fiber_Profiler_Capture_Call *call) {
	if (call->path) {
		free((void*)call->path);
		call->path = NULL;
	}
}

static void Fiber_Profiler_Capture_mark(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct Fiber_Profiler_Capture_Call *call = profiler->calls.base[i];
		rb_gc_mark_movable(call->klass);
	}
}

static void Fiber_Profiler_Capture_compact(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	// If `klass` is stored as a VALUE in calls, we need to update their locations here:
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
			struct Fiber_Profiler_Capture_Call *call = profiler->calls.base[i];
			call->klass = rb_gc_location(call->klass);
	}
}

static void Fiber_Profiler_Capture_free(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	Fiber_Profiler_Array_free(&profiler->calls);
	
	free(profiler);
}

static size_t Fiber_Profiler_Capture_memsize(const void *ptr) {
	const struct Fiber_Profiler_Capture *profiler = (const struct Fiber_Profiler_Capture*)ptr;
	return sizeof(*profiler) + Fiber_Profiler_Array_memory_size(&profiler->calls);
}

const rb_data_type_t Fiber_Profiler_Capture_Type = {
	.wrap_struct_name = "IO::Event::Profiler",
	.function = {
		.dmark = Fiber_Profiler_Capture_mark,
		.dcompact = Fiber_Profiler_Capture_compact,
		.dfree = Fiber_Profiler_Capture_free,
		.dsize = Fiber_Profiler_Capture_memsize,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

struct Fiber_Profiler_Capture *Fiber_Profiler_Capture_get(VALUE self) {
	struct Fiber_Profiler_Capture *profiler;
	TypedData_Get_Struct(self, struct Fiber_Profiler_Capture, &Fiber_Profiler_Capture_Type, profiler);
	return profiler;
}

VALUE Fiber_Profiler_Capture_allocate(VALUE klass) {
	struct Fiber_Profiler_Capture *profiler = ALLOC(struct Fiber_Profiler_Capture);
	
	// Initialize the profiler state:
	profiler->running = 0;
	profiler->capture = 0;
	profiler->stalls = 0;
	profiler->nesting = 0;
	profiler->current = NULL;
	
	profiler->calls.element_initialize = (void (*)(void*))Fiber_Profiler_Capture_Call_initialize;
	profiler->calls.element_free = (void (*)(void*))Fiber_Profiler_Capture_Call_free;
	Fiber_Profiler_Array_initialize(&profiler->calls, 0, sizeof(struct Fiber_Profiler_Capture_Call));
	
	return TypedData_Wrap_Struct(klass, &Fiber_Profiler_Capture_Type, profiler);
}

int Fiber_Profiler_Capture_p(void) {
	const char *enabled = getenv("Fiber_Profiler_Capture");
	
	if (enabled && strcmp(enabled, "true") == 0) {
		return 1;
	}
	
	return 0;
}

float Fiber_Profiler_Capture_default_log_threshold(void) {
	const char *log_threshold = getenv("Fiber_Profiler_Capture_LOG_THRESHOLD");
	
	if (log_threshold) {
		return strtof(log_threshold, NULL);
	} else {
		return 0.01;
	}
}

int Fiber_Profiler_Capture_default_track_calls(void) {
	const char *track_calls = getenv("Fiber_Profiler_Capture_TRACK_CALLS");
	
	if (track_calls && strcmp(track_calls, "false") == 0) {
		return 0;
	} else {
		return 1;
	}
}

VALUE Fiber_Profiler_Capture_initialize(int argc, VALUE *argv, VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	VALUE log_threshold, track_calls;
	
	rb_scan_args(argc, argv, "02", &log_threshold, &track_calls);
	
	if (RB_NIL_P(log_threshold)) {
		profiler->log_threshold = Fiber_Profiler_Capture_default_log_threshold();
	} else {
		profiler->log_threshold = NUM2DBL(log_threshold);
	}
	
	if (RB_NIL_P(track_calls)) {
		profiler->track_calls = Fiber_Profiler_Capture_default_track_calls();
	} else {
		profiler->track_calls = RB_TEST(track_calls);
	}
	
	return self;
}

VALUE Fiber_Profiler_Capture_default(VALUE klass) {
	if (!Fiber_Profiler_Capture_p()) {
		return Qnil;
	}
	
	VALUE profiler = Fiber_Profiler_Capture_allocate(klass);
	
	struct Fiber_Profiler_Capture *profiler_data = Fiber_Profiler_Capture_get(profiler);
	profiler_data->log_threshold = Fiber_Profiler_Capture_default_log_threshold();
	profiler_data->track_calls = Fiber_Profiler_Capture_default_track_calls();
	
	return profiler;
}

VALUE Fiber_Profiler_Capture_new(float log_threshold, int track_calls) {
	VALUE profiler = Fiber_Profiler_Capture_allocate(Fiber_Profiler_Capture);
	
	struct Fiber_Profiler_Capture *profiler_data = Fiber_Profiler_Capture_get(profiler);
	profiler_data->log_threshold = log_threshold;
	profiler_data->track_calls = track_calls;
	
	return profiler;
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_B_CALL);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN);
}

const char *event_flag_name(rb_event_flag_t event_flag) {
	switch (event_flag) {
		case RUBY_EVENT_CALL: return "call";
		case RUBY_EVENT_C_CALL: return "c-call";
		case RUBY_EVENT_B_CALL: return "b-call";
		case RUBY_EVENT_RETURN: return "return";
		case RUBY_EVENT_C_RETURN: return "c-return";
		case RUBY_EVENT_B_RETURN: return "b-return";
		default: return "unknown";
	}
}

static struct Fiber_Profiler_Capture_Call* profiler_event_record_call(struct Fiber_Profiler_Capture *profiler, rb_event_flag_t event_flag, ID id, VALUE klass) {
	struct Fiber_Profiler_Capture_Call *call = Fiber_Profiler_Array_push(&profiler->calls);
	
	call->event_flag = event_flag;

	call->parent = profiler->current;
	profiler->current = call;

	call->nesting = profiler->nesting;
	profiler->nesting += 1;

	if (id) {
		call->id = id;
		call->klass = klass;
	} else {
		rb_frame_method_id_and_class(&call->id, &call->klass);
	}
	
	const char *path = rb_sourcefile();
	if (path) {
		call->path = strdup(path);
	}
	call->line = rb_sourceline();
	
	return call;
}

void Fiber_Profiler_Capture_fiber_switch(struct Fiber_Profiler_Capture *profiler);

static void Fiber_Profiler_Capture_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(data);
	
	if (event_flag & RUBY_EVENT_FIBER_SWITCH) {
		Fiber_Profiler_Capture_fiber_switch(profiler);
		return;
	}
	
	// We don't want to capture data if we're not running:
	if (!profiler->capture) return;
	
	if (event_flag_call_p(event_flag)) {
		struct Fiber_Profiler_Capture_Call *call = profiler_event_record_call(profiler, event_flag, id, klass);
		Fiber_Profiler_Time_current(&call->enter_time);
	}
	
	else if (event_flag_return_p(event_flag)) {
		struct Fiber_Profiler_Capture_Call *call = profiler->current;
		
		// We may encounter returns without a preceeding call. This isn't an error, but we should pretend like the call started at the beginning of the profiling session:
		if (call == NULL) {
			struct Fiber_Profiler_Capture_Call *last_call = Fiber_Profiler_Array_last(&profiler->calls);
			call = profiler_event_record_call(profiler, event_flag, id, klass);
			
			if (last_call) {
				call->enter_time = last_call->enter_time;
			} else {
				call->enter_time = profiler->start_time;
			}
		}
		
		Fiber_Profiler_Time_current(&call->exit_time);
		
		profiler->current = call->parent;
		
		// We may encounter returns without a preceeding call.
		if (profiler->nesting > 0)
			profiler->nesting -= 1;
		
		// If the call was < 0.01% of the total time, we can ignore it:
	}
}

VALUE Fiber_Profiler_Capture_start(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	if (profiler->running) return Qfalse;
	
	profiler->running = 1;
	
	Fiber_Profiler_Capture_reset(profiler);
	Fiber_Profiler_Time_current(&profiler->start_time);
	
	rb_event_flag_t event_flags = RUBY_EVENT_FIBER_SWITCH;
	
	if (profiler->track_calls) {
		event_flags |= RUBY_EVENT_CALL | RUBY_EVENT_RETURN;
		event_flags |= RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN;
		// event_flags |= RUBY_EVENT_B_CALL | RUBY_EVENT_B_RETURN;
	}
	
	VALUE thread = rb_thread_current();
	rb_thread_add_event_hook(thread, Fiber_Profiler_Capture_callback, event_flags, self);
	
	return self;
}

VALUE Fiber_Profiler_Capture_stop(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	if (!profiler->running) return Qfalse;
	
	profiler->running = 0;
	
	VALUE thread = rb_thread_current();
	rb_thread_remove_event_hook_with_data(thread, Fiber_Profiler_Capture_callback, self);
	
	Fiber_Profiler_Time_current(&profiler->stop_time);
	Fiber_Profiler_Capture_reset(profiler);
	
	return self;
}

static inline float Fiber_Profiler_Capture_duration(struct Fiber_Profiler_Capture *profiler) {
	struct timespec duration;
	
	Fiber_Profiler_Time_current(&profiler->stop_time);
	Fiber_Profiler_Time_elapsed(&profiler->start_time, &profiler->stop_time, &duration);
	
	return Fiber_Profiler_Time_duration(&duration);
}

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream);

void Fiber_Profiler_Capture_finish(struct Fiber_Profiler_Capture *profiler) {
	profiler->capture = 0;
	
	struct Fiber_Profiler_Capture_Call *current = profiler->current;
	while (current) {
		Fiber_Profiler_Time_current(&current->exit_time);
		
		current = current->parent;
	}
}

void Fiber_Profiler_Capture_fiber_switch(struct Fiber_Profiler_Capture *profiler)
{
	float duration = Fiber_Profiler_Capture_duration(profiler);
	
	if (profiler->capture) {
		Fiber_Profiler_Capture_finish(profiler);
		
		if (duration > profiler->log_threshold) {
			profiler->stalls += 1;
			Fiber_Profiler_Capture_print(profiler, stderr);
		}
	}
	
	Fiber_Profiler_Capture_reset(profiler);
	
	if (!Fiber_Profiler_Fiber_blocking(Fiber_Profiler_Fiber_current())) {
		// Reset the start time:
		Fiber_Profiler_Time_current(&profiler->start_time);
		
		profiler->capture = 1;
	}
}

static const float Fiber_Profiler_Capture_PRINT_MINIMUM_PROPORTION = 0.01;

void Fiber_Profiler_Capture_print_tty(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream) {
	struct timespec total_duration = {};
	Fiber_Profiler_Time_elapsed(&profiler->start_time, &profiler->stop_time, &total_duration);
	
	fprintf(stderr, "Fiber stalled for %.3f seconds\n", Fiber_Profiler_Time_duration(&total_duration));
	
	size_t skipped = 0;
	
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct Fiber_Profiler_Capture_Call *call = profiler->calls.base[i];
		struct timespec duration = {};
		Fiber_Profiler_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (Fiber_Profiler_Time_proportion(&duration, &total_duration) < Fiber_Profiler_Capture_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		for (size_t i = 0; i < call->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s:%d in %s '%s#%s' (" Fiber_Profiler_TIME_PRINTF_TIMESPEC "s)\n", call->path, call->line, event_flag_name(call->event_flag), RSTRING_PTR(class_inspect), name, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
	}
	
	if (skipped > 0) {
		fprintf(stream, "Skipped %zu calls that were too short to be meaningful.\n", skipped);
	}
}

void Fiber_Profiler_Capture_print_json(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream) {
	struct timespec total_duration = {};
	Fiber_Profiler_Time_elapsed(&profiler->start_time, &profiler->stop_time, &total_duration);
	
	fputc('{', stream);
	
	fprintf(stream, "\"duration\":" Fiber_Profiler_TIME_PRINTF_TIMESPEC, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(total_duration));
	
	size_t skipped = 0;
	
	fprintf(stream, ",\"calls\":[");
	int first = 1;
	
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct Fiber_Profiler_Capture_Call *call = profiler->calls.base[i];
		struct timespec duration = {};
		Fiber_Profiler_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (Fiber_Profiler_Time_proportion(&duration, &total_duration) < Fiber_Profiler_Capture_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s{\"path\":\"%s\",\"line\":%d,\"class\":\"%s\",\"method\":\"%s\",\"duration\":" Fiber_Profiler_TIME_PRINTF_TIMESPEC ",\"nesting\":%zu}", first ? "" : ",", call->path, call->line, RSTRING_PTR(class_inspect), name, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration), call->nesting);
		
		first = 0;
	}
	
	fprintf(stream, "]");
	
	if (skipped > 0) {
		fprintf(stream, ",\"skipped\":%zu", skipped);
	}
	
	fprintf(stream, "}\n");
}

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream) {
	if (isatty(fileno(stream))) {
		Fiber_Profiler_Capture_print_tty(profiler, stream);
	} else {
		Fiber_Profiler_Capture_print_json(profiler, stream);
	}
}

VALUE Fiber_Profiler_Capture_stalls(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	return SIZET2NUM(profiler->stalls);
}

void Init_Fiber_Profiler_Capture(VALUE Fiber_Profiler) {
	Fiber_Profiler_Capture = rb_define_class_under(Fiber_Profiler, "Capture", rb_cObject);
	rb_define_alloc_func(Fiber_Profiler_Capture, Fiber_Profiler_Capture_allocate);
	
	rb_define_singleton_method(Fiber_Profiler_Capture, "default", Fiber_Profiler_Capture_default, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "initialize", Fiber_Profiler_Capture_initialize, -1);
	
	rb_define_method(Fiber_Profiler_Capture, "start", Fiber_Profiler_Capture_start, 0);
	rb_define_method(Fiber_Profiler_Capture, "stop", Fiber_Profiler_Capture_stop, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "stalls", Fiber_Profiler_Capture_stalls, 0);
}
