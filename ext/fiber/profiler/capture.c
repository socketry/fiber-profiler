// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"

#include "time.h"
#include "fiber.h"
#include "deque.h"

#include <stdio.h>
#include <ruby/io.h>
#include <ruby/debug.h>
#include <stdio.h>

enum {
	DEBUG_SKIPPED = 0,
};

int Fiber_Profiler_capture_p = 0;
double Fiber_Profiler_Capture_stall_threshold = 0.01;
int Fiber_Profiler_Capture_track_calls = 1;
double Fiber_Profiler_Capture_sample_rate = 1;

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

struct Fiber_Profiler_Stream {
	FILE *file;
	char *buffer;
	size_t size;
};

void Fiber_Profiler_Stream_initialize(struct Fiber_Profiler_Stream *stream) {
	stream->file = open_memstream(&stream->buffer, &stream->size);
}

void Fiber_Profiler_Stream_free(struct Fiber_Profiler_Stream *stream) {
	if (stream->file) {
		fclose(stream->file);
		stream->file = NULL;
	}
	
	if (stream->buffer) {
		free(stream->buffer);
		stream->buffer = NULL;
	}
}

struct Fiber_Profiler_Capture;
typedef void(*Fiber_Profiler_Stream_Print)(struct Fiber_Profiler_Capture*, FILE* restrict);

struct Fiber_Profiler_Capture {
	// Configuration:
	double stall_threshold;
	int track_calls;
	double sample_rate;
	
	// Calls that are shorter than this filter threshold will be ignored:
	double filter_threshold;
	
	VALUE output;
	Fiber_Profiler_Stream_Print print;
	struct Fiber_Profiler_Stream stream;
	
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
	
	struct Fiber_Profiler_Deque calls;
};

void Fiber_Profiler_Capture_reset(struct Fiber_Profiler_Capture *profiler) {
	profiler->nesting = 0;
	profiler->current = NULL;
	Fiber_Profiler_Deque_truncate(&profiler->calls);
}

void Fiber_Profiler_Capture_Call_initialize(void *element) {
	struct Fiber_Profiler_Capture_Call *call = element;
	
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

void Fiber_Profiler_Capture_Call_free(void *element) {
	struct Fiber_Profiler_Capture_Call *call = element;
	
	if (call->path) {
		free((void*)call->path);
		call->path = NULL;
	}
}

static void Fiber_Profiler_Capture_mark(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	rb_gc_mark_movable(profiler->output);
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	Fiber_Profiler_Deque_each(&profiler->calls, struct Fiber_Profiler_Capture_Call, call) {
		rb_gc_mark_movable(call->klass);
	}
}

static void Fiber_Profiler_Capture_compact(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	profiler->output = rb_gc_location(profiler->output);
	
	// If `klass` is stored as a VALUE in calls, we need to update their locations here:
	Fiber_Profiler_Deque_each(&profiler->calls, struct Fiber_Profiler_Capture_Call, call) {
		call->klass = rb_gc_location(call->klass);
	}
}

static void Fiber_Profiler_Capture_free(void *ptr) {
	struct Fiber_Profiler_Capture *profiler = (struct Fiber_Profiler_Capture*)ptr;
	
	RUBY_ASSERT(profiler->running == 0);
	
	Fiber_Profiler_Stream_free(&profiler->stream);
	Fiber_Profiler_Deque_free(&profiler->calls);
	
	free(profiler);
}

static size_t Fiber_Profiler_Capture_memsize(const void *ptr) {
	const struct Fiber_Profiler_Capture *profiler = (const struct Fiber_Profiler_Capture*)ptr;
	return sizeof(*profiler) + Fiber_Profiler_Deque_memory_size(&profiler->calls);
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

int IO_istty(VALUE io) {
	if (RB_TYPE_P(io, T_FILE)) {
		int descriptor = rb_io_descriptor(io);
		return isatty(descriptor);
	}
	
	return 0;
}

void Fiber_Profiler_Capture_print_tty(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream);
void Fiber_Profiler_Capture_print_json(struct Fiber_Profiler_Capture *profiler, FILE *restrict stream);

static void Fiber_Profiler_Capture_output_set(struct Fiber_Profiler_Capture *profiler, VALUE output) {
	profiler->output = output;
	
	if (IO_istty(profiler->output)) {
		profiler->print = &Fiber_Profiler_Capture_print_tty;
	} else {
		profiler->print = &Fiber_Profiler_Capture_print_json;
	}
}

VALUE Fiber_Profiler_Capture_allocate(VALUE klass) {
	struct Fiber_Profiler_Capture *profiler = ALLOC(struct Fiber_Profiler_Capture);
	
	// Initialize the profiler state:
	Fiber_Profiler_Stream_initialize(&profiler->stream);
	profiler->output = Qnil;
	profiler->running = 0;
	profiler->capture = 0;
	profiler->stalls = 0;
	profiler->nesting = 0;
	profiler->current = NULL;
	
	profiler->stall_threshold = Fiber_Profiler_Capture_stall_threshold;
	profiler->track_calls = Fiber_Profiler_Capture_track_calls;
	profiler->sample_rate = Fiber_Profiler_Capture_sample_rate;
	
	// Filter calls that are less than 1% of the stall threshold:
	profiler->filter_threshold = profiler->stall_threshold * 0.01;
	
	profiler->calls.element_initialize = (void (*)(void*))Fiber_Profiler_Capture_Call_initialize;
	profiler->calls.element_free = (void (*)(void*))Fiber_Profiler_Capture_Call_free;
	Fiber_Profiler_Deque_initialize(&profiler->calls, sizeof(struct Fiber_Profiler_Capture_Call));
	
	return TypedData_Wrap_Struct(klass, &Fiber_Profiler_Capture_Type, profiler);
}

ID Fiber_Profiler_Capture_initialize_options[4];

VALUE Fiber_Profiler_Capture_initialize(int argc, VALUE *argv, VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	VALUE arguments[4] = {0};
	VALUE options = Qnil;
	rb_scan_args(argc, argv, ":", &options);
	rb_get_kwargs(options, Fiber_Profiler_Capture_initialize_options, 0, 4, arguments);
	
	if (arguments[0] != Qundef) {
		profiler->stall_threshold = NUM2DBL(arguments[0]);
	}
	
	if (arguments[1] != Qundef) {
		profiler->track_calls = RB_TEST(arguments[1]);
	}
	
	if (arguments[2] != Qundef) {
		profiler->sample_rate = NUM2DBL(arguments[2]);
	}
	
	if (arguments[3] != Qundef) {
		Fiber_Profiler_Capture_output_set(profiler, arguments[3]);
	} else {
		// Initialize the profiler output - we dup `rb_stderr` because the profiler may otherwise run into synchronization issues with other uses of `rb_stderr`:
		Fiber_Profiler_Capture_output_set(profiler, rb_obj_dup(rb_stderr));
	}
	
	return self;
}

VALUE Fiber_Profiler_Capture_default(VALUE klass) {
	if (!Fiber_Profiler_capture_p) {
		return Qnil;
	}
	
	VALUE profiler = Fiber_Profiler_Capture_allocate(klass);
	Fiber_Profiler_Capture_initialize(0, NULL, profiler);
	
	return profiler;
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_B_CALL | RUBY_INTERNAL_EVENT_GC_START);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN | RUBY_INTERNAL_EVENT_GC_END_SWEEP);
}

const char *event_flag_name(rb_event_flag_t event_flag) {
	switch (event_flag) {
		case RUBY_EVENT_CALL: return "call";
		case RUBY_EVENT_C_CALL: return "c-call";
		case RUBY_EVENT_B_CALL: return "b-call";
		case RUBY_EVENT_RETURN: return "return";
		case RUBY_EVENT_C_RETURN: return "c-return";
		case RUBY_EVENT_B_RETURN: return "b-return";
		case RUBY_INTERNAL_EVENT_GC_START: return "gc-start";
		case RUBY_INTERNAL_EVENT_GC_END_MARK: return "gc-end-mark";
		case RUBY_INTERNAL_EVENT_GC_END_SWEEP: return "gc-end-sweep";
		default: return "unknown";
	}
}

static struct Fiber_Profiler_Capture_Call* profiler_event_record_call(struct Fiber_Profiler_Capture *profiler, rb_event_flag_t event_flag, ID id, VALUE klass) {
	struct Fiber_Profiler_Capture_Call *call = Fiber_Profiler_Deque_push(&profiler->calls);
	
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
			struct Fiber_Profiler_Capture_Call *last_call = Fiber_Profiler_Deque_last(&profiler->calls);
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
		
		// If the call was < 1% of the stall threshold, we can ignore it:
		double duration = Fiber_Profiler_Time_delta(&call->enter_time, &call->exit_time);
		if (duration < profiler->filter_threshold) {
			Fiber_Profiler_Deque_pop(&profiler->calls);
		}
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
	
	// if (profiler->track_garbage_collection) {
		rb_thread_add_event_hook(thread, Fiber_Profiler_Capture_callback, RUBY_INTERNAL_EVENT_GC_START | RUBY_INTERNAL_EVENT_GC_END_SWEEP, self);
	// }
	
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

void Fiber_Profiler_Capture_finish(struct Fiber_Profiler_Capture *profiler) {
	profiler->capture = 0;
	
	struct Fiber_Profiler_Capture_Call *current = profiler->current;
	while (current) {
		Fiber_Profiler_Time_current(&current->exit_time);
		
		current = current->parent;
	}
}

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *profiler);

int Fiber_Profiler_Capture_sample(struct Fiber_Profiler_Capture *profiler) {
	VALUE fiber = Fiber_Profiler_Fiber_current();
	
	// We don't want to capture data from blocking fibers:
	if (Fiber_Profiler_Fiber_blocking(fiber)) return 0;
	
	if (profiler->sample_rate < 1) {
		return rand() < (RAND_MAX * profiler->sample_rate);
	} else {
		return 1;
	}
}

void Fiber_Profiler_Capture_fiber_switch(struct Fiber_Profiler_Capture *profiler)
{
	float duration = Fiber_Profiler_Capture_duration(profiler);
	
	if (profiler->capture) {
		Fiber_Profiler_Capture_finish(profiler);
		
		if (duration > profiler->stall_threshold) {
			profiler->stalls += 1;
			Fiber_Profiler_Capture_print(profiler);
		}
	}
	
	Fiber_Profiler_Capture_reset(profiler);
	
	if (Fiber_Profiler_Capture_sample(profiler)) {
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
	
	Fiber_Profiler_Deque_each(&profiler->calls, struct Fiber_Profiler_Capture_Call, call) {
		struct timespec duration = {};
		Fiber_Profiler_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (Fiber_Profiler_Time_proportion(&duration, &total_duration) < Fiber_Profiler_Capture_PRINT_MINIMUM_PROPORTION) {
			
			if (!DEBUG_SKIPPED) {
				skipped += 1;
				continue;
			} else {
				fprintf(stream, "\e[2m");
			}
		}
		
		for (size_t i = 0; i < call->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s:%d in %s '%s#%s' (" Fiber_Profiler_TIME_PRINTF_TIMESPEC "s)\n", call->path, call->line, event_flag_name(call->event_flag), RSTRING_PTR(class_inspect), name, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
		
		if (DEBUG_SKIPPED) {
			fprintf(stream, "\e[0m");
		}
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
	
	Fiber_Profiler_Deque_each(&profiler->calls, struct Fiber_Profiler_Capture_Call, call) {
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

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *profiler) {
	if (profiler->output == Qnil) return;
	
	FILE *stream = profiler->stream.file;
	profiler->print(profiler, stream);
	fflush(stream);
	
	rb_io_write(profiler->output, rb_str_new_static(profiler->stream.buffer, profiler->stream.size));
	
	fseek(stream, 0, SEEK_SET);
}

#pragma mark - Accessors

static VALUE Fiber_Profiler_Capture_stall_threshold_get(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	return DBL2NUM(profiler->stall_threshold);
}

static VALUE Fiber_Profiler_Capture_track_calls_get(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	return profiler->track_calls ? Qtrue : Qfalse;
}

static VALUE Fiber_Profiler_Capture_stalls_get(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	return SIZET2NUM(profiler->stalls);
}

static VALUE Fiber_Profiler_Capture_sample_rate_get(VALUE self) {
	struct Fiber_Profiler_Capture *profiler = Fiber_Profiler_Capture_get(self);
	
	return DBL2NUM(profiler->sample_rate);
}

#pragma mark - Environment Variables

static int FIBER_PROFILER_CAPTURE(void) {
	const char *value = getenv("FIBER_PROFILER_CAPTURE");
	
	if (value && strcmp(value, "true") == 0) {
		return 1;
	} else {
		return 0;
	}
}

static double FIBER_PROFILER_CAPTURE_STALL_THRESHOLD(void) {
	const char *value = getenv("FIBER_PROFILER_CAPTURE_STALL_THRESHOLD");
	
	if (value) {
		return atof(value);
	} else {
		return 0.01;
	}
}

static int FIBER_PROFILER_CAPTURE_TRACK_CALLS(void) {
	const char *value = getenv("FIBER_PROFILER_CAPTURE_TRACK_CALLS");
	
	if (value && strcmp(value, "false") == 0) {
		return 0;
	} else {
		return 1;
	}
}

static double FIBER_PROFILER_CAPTURE_SAMPLE_RATE(void) {
	const char *value = getenv("FIBER_PROFILER_CAPTURE_SAMPLE_RATE");
	
	if (value) {
		return atof(value);
	} else {
		return 1;
	}
}

#pragma mark - Initialization

void Init_Fiber_Profiler_Capture(VALUE Fiber_Profiler) {
	Fiber_Profiler_capture_p = FIBER_PROFILER_CAPTURE();
	Fiber_Profiler_Capture_stall_threshold = FIBER_PROFILER_CAPTURE_STALL_THRESHOLD();
	Fiber_Profiler_Capture_track_calls = FIBER_PROFILER_CAPTURE_TRACK_CALLS();
	Fiber_Profiler_Capture_sample_rate = FIBER_PROFILER_CAPTURE_SAMPLE_RATE();
	
	Fiber_Profiler_Capture_initialize_options[0] = rb_intern("stall_threshold");
	Fiber_Profiler_Capture_initialize_options[1] = rb_intern("track_calls");
	Fiber_Profiler_Capture_initialize_options[2] = rb_intern("sample_rate");
	Fiber_Profiler_Capture_initialize_options[3] = rb_intern("output");
	
	Fiber_Profiler_Capture = rb_define_class_under(Fiber_Profiler, "Capture", rb_cObject);
	rb_define_alloc_func(Fiber_Profiler_Capture, Fiber_Profiler_Capture_allocate);
	
	rb_define_singleton_method(Fiber_Profiler_Capture, "default", Fiber_Profiler_Capture_default, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "initialize", Fiber_Profiler_Capture_initialize, -1);
	
	rb_define_method(Fiber_Profiler_Capture, "start", Fiber_Profiler_Capture_start, 0);
	rb_define_method(Fiber_Profiler_Capture, "stop", Fiber_Profiler_Capture_stop, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "stall_threshold", Fiber_Profiler_Capture_stall_threshold_get, 0);
	rb_define_method(Fiber_Profiler_Capture, "track_calls", Fiber_Profiler_Capture_track_calls_get, 0);
	rb_define_method(Fiber_Profiler_Capture, "sample_rate", Fiber_Profiler_Capture_sample_rate_get, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "stalls", Fiber_Profiler_Capture_stalls_get, 0);
	
	rb_define_singleton_method(Fiber_Profiler_Capture, "default", Fiber_Profiler_Capture_default, 0);
}
