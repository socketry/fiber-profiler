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
	DEBUG_FILTERED = 0,
};

int Fiber_Profiler_capture_p = 0;
double Fiber_Profiler_Capture_stall_threshold = 0.01;
double Fiber_Profiler_Capture_filter_threshold = 0.001;
int Fiber_Profiler_Capture_track_calls = 1;
double Fiber_Profiler_Capture_sample_rate = 1;

VALUE Fiber_Profiler_Capture = Qnil;

struct Fiber_Profiler_Capture_Call {
	struct timespec enter_time;
	double duration;
	
	int nesting;
	size_t children;
	size_t filtered;
	
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
typedef void(*Fiber_Profiler_Stream_Print)(struct Fiber_Profiler_Capture*, FILE* restrict, double duration);

struct Fiber_Profiler_Capture {
	// The threshold in seconds, which determines when a fiber is considered to have stalled the event loop.
	double stall_threshold;
	
	// Whether or not to track calls.
	int track_calls;
	
	// The sample rate of the capture, as a fraction of 1.0, which controls how often the profiler will sample between fiber context switches.
	double sample_rate;
	
	// Calls that are shorter than this filter threshold will be ignored.
	double filter_threshold;
	
	// The output object to write to.
	VALUE output;
	
	// The stream print function to use.
	Fiber_Profiler_Stream_Print print;
	
	// The stream buffer used for printing.
	struct Fiber_Profiler_Stream stream;
	
	// How many fiber context switches have been encountered. Not all of them will be sampled, based on the sample rate.
	size_t switches;

	// How many samples have been taken, not all of them will be stalls, based on the stall threshold.
	size_t samples;
	
	// The number of stalls encountered and printed.
	size_t stalls;
	
	// Whether or not the profiler is currently running.
	int running;
	
	// The thread being profiled.
	VALUE thread;
	
	// Whether or not to capture call data.
	int capture;
	
	// The start time of the profile.
	struct timespec start_time;
	
	// The time of the last fiber switch that was sampled.
	struct timespec switch_time;
	
	// The depth of the call stack (can be negative).
	int nesting;
	
	// The minimum nesting level encountered during the profiling session.
	int nesting_minimum;
	
	// The current call frame.
	struct Fiber_Profiler_Capture_Call *current;
	
	// The call recorded during the profiling session.
	struct Fiber_Profiler_Deque calls;
};

void Fiber_Profiler_Capture_Call_initialize(void *element) {
	struct Fiber_Profiler_Capture_Call *call = element;
	
	call->enter_time.tv_sec = 0;
	call->enter_time.tv_nsec = 0;
	call->duration = 0;
	
	call->nesting = 0;
	call->children = 0;
	call->filtered = 0;
	
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
	struct Fiber_Profiler_Capture *capture = (struct Fiber_Profiler_Capture*)ptr;
	
	rb_gc_mark_movable(capture->thread);
	rb_gc_mark_movable(capture->output);
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	Fiber_Profiler_Deque_each(&capture->calls, struct Fiber_Profiler_Capture_Call, call) {
		rb_gc_mark_movable(call->klass);
	}
}

static void Fiber_Profiler_Capture_compact(void *ptr) {
	struct Fiber_Profiler_Capture *capture = (struct Fiber_Profiler_Capture*)ptr;
	
	capture->thread = rb_gc_location(capture->thread);
	capture->output = rb_gc_location(capture->output);
	
	// If `klass` is stored as a VALUE in calls, we need to update their locations here:
	Fiber_Profiler_Deque_each(&capture->calls, struct Fiber_Profiler_Capture_Call, call) {
		call->klass = rb_gc_location(call->klass);
	}
}

static void Fiber_Profiler_Capture_free(void *ptr) {
	struct Fiber_Profiler_Capture *capture = (struct Fiber_Profiler_Capture*)ptr;
	
	RUBY_ASSERT(capture->running == 0);
	
	Fiber_Profiler_Stream_free(&capture->stream);
	Fiber_Profiler_Deque_free(&capture->calls);
	
	free(capture);
}

static size_t Fiber_Profiler_Capture_memsize(const void *ptr) {
	const struct Fiber_Profiler_Capture *capture = (const struct Fiber_Profiler_Capture*)ptr;
	return sizeof(*capture) + Fiber_Profiler_Deque_memory_size(&capture->calls);
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
	struct Fiber_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Fiber_Profiler_Capture, &Fiber_Profiler_Capture_Type, capture);
	return capture;
}

int IO_istty(VALUE io) {
	if (RB_TYPE_P(io, T_FILE)) {
		int descriptor = rb_io_descriptor(io);
		return isatty(descriptor);
	}
	
	return 0;
}

void Fiber_Profiler_Capture_print_tty(struct Fiber_Profiler_Capture *capture, FILE *restrict stream, double duration);
void Fiber_Profiler_Capture_print_json(struct Fiber_Profiler_Capture *capture, FILE *restrict stream, double duration);

static void Fiber_Profiler_Capture_output_set(struct Fiber_Profiler_Capture *capture, VALUE output) {
	capture->output = output;
	
	if (IO_istty(capture->output)) {
		capture->print = &Fiber_Profiler_Capture_print_tty;
	} else {
		capture->print = &Fiber_Profiler_Capture_print_json;
	}
}

VALUE Fiber_Profiler_Capture_allocate(VALUE klass) {
	struct Fiber_Profiler_Capture *capture = ALLOC(struct Fiber_Profiler_Capture);
	
	// Initialize the profiler state:
	Fiber_Profiler_Stream_initialize(&capture->stream);
	capture->output = Qnil;
	
	capture->switches = 0;
	capture->samples = 0;
	capture->stalls = 0;
	
	capture->running = 0;
	capture->thread = Qnil;
	
	capture->capture = 0;
	capture->nesting = 0;
	capture->nesting_minimum = 0;
	capture->current = NULL;
	
	capture->stall_threshold = Fiber_Profiler_Capture_stall_threshold;
	capture->filter_threshold = Fiber_Profiler_Capture_filter_threshold;
	capture->track_calls = Fiber_Profiler_Capture_track_calls;
	capture->sample_rate = Fiber_Profiler_Capture_sample_rate;
	
	capture->calls.element_initialize = (void (*)(void*))Fiber_Profiler_Capture_Call_initialize;
	capture->calls.element_free = (void (*)(void*))Fiber_Profiler_Capture_Call_free;
	
	Fiber_Profiler_Deque_initialize(&capture->calls, sizeof(struct Fiber_Profiler_Capture_Call));
	Fiber_Profiler_Deque_reserve_default(&capture->calls);
	
	return TypedData_Wrap_Struct(klass, &Fiber_Profiler_Capture_Type, capture);
}

ID Fiber_Profiler_Capture_initialize_options[5];

VALUE Fiber_Profiler_Capture_initialize(int argc, VALUE *argv, VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	VALUE arguments[5] = {0};
	VALUE options = Qnil;
	rb_scan_args(argc, argv, ":", &options);
	rb_get_kwargs(options, Fiber_Profiler_Capture_initialize_options, 0, 5, arguments);
	
	if (arguments[0] != Qundef) {
		capture->stall_threshold = NUM2DBL(arguments[0]);
	}
	
	if (arguments[1] != Qundef) {
		capture->filter_threshold = NUM2DBL(arguments[1]);
	}
	
	if (arguments[2] != Qundef) {
		capture->track_calls = RB_TEST(arguments[2]);
	}
	
	if (arguments[3] != Qundef) {
		capture->sample_rate = NUM2DBL(arguments[3]);
	}
	
	if (arguments[4] != Qundef) {
		Fiber_Profiler_Capture_output_set(capture, arguments[4]);
	} else {
		// Initialize the profiler output - we dup `rb_stderr` because the profiler may otherwise run into synchronization issues with other uses of `rb_stderr`:
		Fiber_Profiler_Capture_output_set(capture, rb_obj_dup(rb_stderr));
	}
	
	return self;
}

VALUE Fiber_Profiler_Capture_default(VALUE klass) {
	if (!Fiber_Profiler_capture_p) {
		return Qnil;
	}
	
	VALUE capture = Fiber_Profiler_Capture_allocate(klass);
	Fiber_Profiler_Capture_initialize(0, NULL, capture);
	
	return capture;
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
		case RUBY_EVENT_LINE: return "line";
		default: return "unknown";
	}
}

static struct Fiber_Profiler_Capture_Call* Fiber_Profiler_Capture_Call_new(struct Fiber_Profiler_Capture *capture, rb_event_flag_t event_flag, ID id, VALUE klass) {
	struct Fiber_Profiler_Capture_Call *call = Fiber_Profiler_Deque_push(&capture->calls);
	
	call->event_flag = event_flag;

	call->parent = capture->current;
	if (call->parent) {
		call->parent->children += 1;
	}
	
	capture->current = call;
	
	call->nesting = capture->nesting;

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

// Finish the call by calculating the duration and filtering it if necessary.
int Fiber_Profiler_Capture_Call_finish(struct Fiber_Profiler_Capture *capture, struct Fiber_Profiler_Capture_Call *call) {
	// Don't filter calls if we're not running:
	if (DEBUG_FILTERED) return 0;
	
	if (event_flag_return_p(call->event_flag)) {
		// We don't filter return statements, as they are always part of the call stack:
		return 0;
	}
	
	if (call->duration < capture->filter_threshold) {
		// We can only remove calls from the end of the deque, otherwise they might be referenced by other calls:
		if (call == Fiber_Profiler_Deque_last(&capture->calls)) {
			if (capture->current == call) {
				capture->current = call->parent;
			}
			
			if (call->parent) {
				call->parent->children -= 1;
				call->parent->filtered += 1;
				call->parent = NULL;
			}
			
			Fiber_Profiler_Deque_pop(&capture->calls);
			
			return 1;
		}
	}
	
	return 0;
}

// Whether to highlight a call as expensive. This is purely cosmetic.
static const double Fiber_Profiler_Capture_Call_EXPENSIVE_THRESHOLD = 0.2;

int Fiber_Profiler_Capture_Call_expensive_p(struct Fiber_Profiler_Capture *capture, struct Fiber_Profiler_Capture_Call *call, double duration) {
	if (call->duration > duration * Fiber_Profiler_Capture_Call_EXPENSIVE_THRESHOLD) {
		return 1;
	}
	
	return 0;
}

static void Fiber_Profiler_Capture_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(data);
	
	// We don't want to capture data if we're not running:
	if (!capture->capture) return;
	
	if (event_flag_call_p(event_flag)) {
		struct Fiber_Profiler_Capture_Call *call = Fiber_Profiler_Capture_Call_new(capture, event_flag, id, klass);
		
		capture->nesting += 1;
		
		Fiber_Profiler_Time_current(&call->enter_time);
	}
	
	else if (event_flag_return_p(event_flag)) {
		struct Fiber_Profiler_Capture_Call *call = capture->current;
		
		// We may encounter returns without a preceeding call. This isn't an error, but we should pretend like the call started at the beginning of the profiling session:
		if (call == NULL) {
			struct Fiber_Profiler_Capture_Call *last_call = Fiber_Profiler_Deque_last(&capture->calls);
			call = Fiber_Profiler_Capture_Call_new(capture, event_flag, id, klass);
			
			struct timespec call_time;
			
			if (last_call) {
				call_time = last_call->enter_time;
			} else {
				call_time = capture->switch_time;
			}
			
			// For return statements, we record the current time as the enter time:
			Fiber_Profiler_Time_current(&call->enter_time);
			call->duration = Fiber_Profiler_Time_delta(&call_time, &call->enter_time);
		} else {
			call->duration = Fiber_Profiler_Time_delta_current(&call->enter_time);
		}
		
		capture->current = call->parent;
		
		// We may encounter returns without a preceeding call.
		capture->nesting -= 1;
		
		// We need to keep track of how deep the call stack goes:
		if (capture->nesting < capture->nesting_minimum) {
			capture->nesting_minimum = capture->nesting;
		}
		
		Fiber_Profiler_Capture_Call_finish(capture, call);
	}
	
	else {
		struct Fiber_Profiler_Capture_Call *last_call = Fiber_Profiler_Deque_last(&capture->calls);
		struct Fiber_Profiler_Capture_Call *call = Fiber_Profiler_Capture_Call_new(capture, event_flag, id, klass);
		
		if (last_call) {
			call->enter_time = last_call->enter_time;
		} else {
			call->enter_time = capture->switch_time;
		}
		
		call->duration = Fiber_Profiler_Time_delta_current(&call->enter_time);
	}
}

void Fiber_Profiler_Capture_pause(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	if (!capture->capture) return;
	capture->capture = 0;
	
	if (capture->track_calls) {
		rb_thread_remove_event_hook_with_data(capture->thread, Fiber_Profiler_Capture_callback, self);
	}
}

void Fiber_Profiler_Capture_resume(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	if (capture->capture) return;
	capture->capture = 1;
	capture->samples += 1;
	
	if (capture->track_calls) {
		rb_event_flag_t event_flags = 0;
		
		// event_flags |= RUBY_EVENT_LINE;
		event_flags |= RUBY_EVENT_CALL | RUBY_EVENT_RETURN;
		event_flags |= RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN;
		event_flags |= RUBY_EVENT_B_CALL | RUBY_EVENT_B_RETURN;
		
		// CRuby will raise an exception if you try to add "INTERNAL_EVENT" hooks at the same time as other hooks, so we do it in two calls:
		rb_thread_add_event_hook(capture->thread, Fiber_Profiler_Capture_callback, event_flags, self);
		rb_thread_add_event_hook(capture->thread, Fiber_Profiler_Capture_callback, RUBY_INTERNAL_EVENT_GC_START | RUBY_INTERNAL_EVENT_GC_END_SWEEP, self);
	}
}

void Fiber_Profiler_Capture_fiber_switch(VALUE self);

void Fiber_Profiler_Capture_fiber_switch_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	Fiber_Profiler_Capture_fiber_switch(data);
}

// Reset the sample state, and truncate the call log.
void Fiber_Profiler_Capture_reset(struct Fiber_Profiler_Capture *capture) {
	capture->nesting = 0;
	capture->nesting_minimum = 0;
	capture->current = NULL;
	Fiber_Profiler_Deque_truncate(&capture->calls);
}

VALUE Fiber_Profiler_Capture_start(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	if (capture->running) return Qfalse;
	
	capture->running = 1;
	capture->thread = rb_thread_current();
	
	Fiber_Profiler_Capture_reset(capture);
	Fiber_Profiler_Time_current(&capture->start_time);
	
	rb_thread_add_event_hook(capture->thread, Fiber_Profiler_Capture_fiber_switch_callback, RUBY_EVENT_FIBER_SWITCH, self);
	
	return self;
}

VALUE Fiber_Profiler_Capture_stop(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	if (!capture->running) return Qfalse;
	
	Fiber_Profiler_Capture_pause(self);
	
	rb_thread_remove_event_hook_with_data(capture->thread, Fiber_Profiler_Capture_fiber_switch_callback, self);
	
	capture->running = 0;
	capture->thread = Qnil;
	
	Fiber_Profiler_Capture_reset(capture);
	
	return self;
}

void Fiber_Profiler_Capture_finish(struct Fiber_Profiler_Capture *capture, struct timespec switch_time) {
	struct Fiber_Profiler_Capture_Call *current = capture->current;
	while (current) {
		struct Fiber_Profiler_Capture_Call *parent = current->parent;
		
		current->duration = Fiber_Profiler_Time_delta(&current->enter_time, &switch_time);
		
		Fiber_Profiler_Capture_Call_finish(capture, current);
		
		current = parent;
	}
}

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *capture, double duration);

int Fiber_Profiler_Capture_sample(struct Fiber_Profiler_Capture *capture) {
	VALUE fiber = Fiber_Profiler_Fiber_current();
	
	// We don't want to capture data from blocking fibers:
	if (Fiber_Profiler_Fiber_blocking(fiber)) return 0;
	
	if (capture->sample_rate < 1) {
		return rand() < (RAND_MAX * capture->sample_rate);
	} else {
		return 1;
	}
}

void Fiber_Profiler_Capture_fiber_switch(VALUE self)
{
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	capture->switches += 1;
	
	if (capture->capture) {
		// The time of the switch (end):
		struct timespec switch_time;
		Fiber_Profiler_Time_current(&switch_time);
	
		// The duration of the sample:
		double duration = Fiber_Profiler_Time_delta(&capture->switch_time, &switch_time);
		
		// Finish the current sample:
		Fiber_Profiler_Capture_pause(self);
		Fiber_Profiler_Capture_finish(capture, switch_time);
		
		// If the duration of the sample is greater than the stall threshold, we consider it a stall:
		if (duration > capture->stall_threshold) {
			capture->stalls += 1;
			
			// Print the sample:
			Fiber_Profiler_Capture_print(capture, duration);
		}
		
		// Reset the capture state:
		Fiber_Profiler_Capture_reset(capture);
	}
	
	if (Fiber_Profiler_Capture_sample(capture)) {
		// Capture the time of the switch (start):
		Fiber_Profiler_Time_current(&capture->switch_time);
		
		// Start capturing data again:
		Fiber_Profiler_Capture_resume(self);
	}
}

// When sampling a fiber, we may encounter returns without a preceeding call. This isn't an error, and we should correctly visualize the call stack. We track both the relative nesting (which can be negative) and the minimum nesting level encountered during the profiling session, and use that to determine the absolute nesting level of each call when printing the call stack.
static size_t Fiber_Profiler_Capture_absolute_nesting(struct Fiber_Profiler_Capture *capture, struct Fiber_Profiler_Capture_Call *call) {
	return call->nesting - capture->nesting_minimum;
}

// If a call is within this threshold of the parent call, it will be skipped when printing the call stack - it's considered inconsequential to the performance of the parent call.
static const double Fiber_Profiler_Capture_SKIP_THRESHOLD = 0.98;

void Fiber_Profiler_Capture_print_tty(struct Fiber_Profiler_Capture *capture, FILE *restrict stream, double duration) {
	double start_time = Fiber_Profiler_Time_delta(&capture->start_time, &capture->switch_time);
	
	fprintf(stderr, "## Fiber stalled for %.3f seconds (switches=%zu, samples=%zu, stalls=%zu, T+%0.3fs)\n", duration, capture->switches, capture->samples, capture->stalls, start_time);
	
	size_t skipped = 0;
	
	Fiber_Profiler_Deque_each(&capture->calls, struct Fiber_Profiler_Capture_Call, call) {
		if (call->children) {
			if (call->parent && call->parent->children == 1) {
				if (call->duration > call->parent->duration * Fiber_Profiler_Capture_SKIP_THRESHOLD) {
					if (!DEBUG_SKIPPED) {
						// We remove the nesting level as we're skipping this call - and we use this to track the nesting of child calls which MAY be printed:
						call->nesting = call->parent->nesting;
						skipped += 1;
						continue;
					} else {
						fprintf(stream, "\e[34m");
					}
				}
			}
		}
		
		if (call->parent) {
			call->nesting = call->parent->nesting + 1;
		}
		
		if (skipped) {
			fprintf(stream, "\e[2m");
			
			size_t nesting = Fiber_Profiler_Capture_absolute_nesting(capture, call);
			for (size_t i = 0; i < nesting; i += 1) {
				fputc('\t', stream);
			}
			
			fprintf(stream, "... skipped %zu nested calls ...\e[0m\n", skipped);
			
			skipped = 0;
			call->nesting += 1;
		}
		
		size_t nesting = Fiber_Profiler_Capture_absolute_nesting(capture, call);
		for (size_t i = 0; i < nesting; i += 1) {
			fputc('\t', stream);
		}
		
		if (Fiber_Profiler_Capture_Call_expensive_p(capture, call, duration)) {
			fprintf(stream, "\e[31m");
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		struct timespec offset;
		Fiber_Profiler_Time_elapsed(&capture->switch_time, &call->enter_time, &offset);
		
		fprintf(stream, "%s:%d in %s '%s#%s' (%0.4fs, T+" Fiber_Profiler_TIME_PRINTF_TIMESPEC ")\n", call->path, call->line, event_flag_name(call->event_flag), RSTRING_PTR(class_inspect), name, call->duration, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(offset));
		
		fprintf(stream, "\e[0m");
		
		if (call->filtered) {
			fprintf(stream, "\e[2m");
			
			for (size_t i = 0; i < nesting + 1; i += 1) {
				fputc('\t', stream);
			}
			
			fprintf(stream, "... filtered %zu direct calls ...\e[0m\n", call->filtered);
		}
	}
	
	if (skipped) {
		fprintf(stream, "\e[2m... skipped %zu calls ...\e[0m\n", skipped);
	}
}

void Fiber_Profiler_Capture_print_json(struct Fiber_Profiler_Capture *capture, FILE *restrict stream, double duration) {
	double start_time = Fiber_Profiler_Time_delta(&capture->start_time, &capture->switch_time);
	
	fputc('{', stream);
	
	fprintf(stream, "\"start_time\":%0.3f,\"duration\":%0.6f", start_time, duration);
	
	size_t skipped = 0;
	
	fprintf(stream, ",\"calls\":[");
	int first = 1;
	
	Fiber_Profiler_Deque_each(&capture->calls, struct Fiber_Profiler_Capture_Call, call) {
		if (call->children) {
			if (call->parent && call->parent->children == 1) {
				if (call->duration > call->parent->duration * Fiber_Profiler_Capture_SKIP_THRESHOLD) {
					// We remove the nesting level as we're skipping this call - and we use this to track the nesting of child calls which MAY be printed:
					call->nesting = call->parent->nesting;
					skipped += 1;
					continue;
				}
			}
		}
		
		if (call->parent) {
			call->nesting = call->parent->nesting + 1;
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		size_t nesting = Fiber_Profiler_Capture_absolute_nesting(capture, call);
		
		struct timespec offset;
		Fiber_Profiler_Time_elapsed(&capture->switch_time, &call->enter_time, &offset);
		
		fprintf(stream, "%s{\"path\":\"%s\",\"line\":%d,\"class\":\"%s\",\"method\":\"%s\",\"duration\":%0.6f,\"offset\":" Fiber_Profiler_TIME_PRINTF_TIMESPEC ",\"nesting\":%zu,\"skipped\":%zu,\"filtered\":%zu}", first ? "" : ",", call->path, call->line, RSTRING_PTR(class_inspect), name, call->duration, Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(offset), nesting, skipped, call->filtered);
		
		skipped = 0;
		first = 0;
	}
	
	fprintf(stream, "]");
	
	if (skipped > 0) {
		fprintf(stream, ",\"skipped\":%zu", skipped);
	}
	
	fprintf(stream, ",\"switches\":%zu,\"samples\":%zu,\"stalls\":%zu}\n", capture->switches, capture->samples, capture->stalls);
}

void Fiber_Profiler_Capture_print(struct Fiber_Profiler_Capture *capture, double duration) {
	if (capture->output == Qnil) return;
	
	FILE *stream = capture->stream.file;
	capture->print(capture, stream, duration);
	fflush(stream);
	
	rb_io_write(capture->output, rb_str_new_static(capture->stream.buffer, capture->stream.size));
	
	fseek(stream, 0, SEEK_SET);
}

#pragma mark - Accessors

static VALUE Fiber_Profiler_Capture_stall_threshold_get(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	return DBL2NUM(capture->stall_threshold);
}

static VALUE Fiber_Profiler_Capture_filter_threshold_get(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	return DBL2NUM(capture->filter_threshold);
}

static VALUE Fiber_Profiler_Capture_track_calls_get(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	return capture->track_calls ? Qtrue : Qfalse;
}

static VALUE Fiber_Profiler_Capture_stalls_get(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	return SIZET2NUM(capture->stalls);
}

static VALUE Fiber_Profiler_Capture_sample_rate_get(VALUE self) {
	struct Fiber_Profiler_Capture *capture = Fiber_Profiler_Capture_get(self);
	
	return DBL2NUM(capture->sample_rate);
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

static double FIBER_PROFILER_CAPTURE_FILTER_THRESHOLD(void) {
	const char *value = getenv("FIBER_PROFILER_CAPTURE_FILTER_THRESHOLD");
	
	if (value) {
		return atof(value);
	} else {
		// We use 10% of the stall threshold as the default filter threshold:
		return Fiber_Profiler_Capture_stall_threshold * 0.1;
	}
}

#pragma mark - Initialization

void Init_Fiber_Profiler_Capture(VALUE Fiber_Profiler) {
	Fiber_Profiler_capture_p = FIBER_PROFILER_CAPTURE();
	Fiber_Profiler_Capture_stall_threshold = FIBER_PROFILER_CAPTURE_STALL_THRESHOLD();
	Fiber_Profiler_Capture_filter_threshold = FIBER_PROFILER_CAPTURE_FILTER_THRESHOLD();
	Fiber_Profiler_Capture_track_calls = FIBER_PROFILER_CAPTURE_TRACK_CALLS();
	Fiber_Profiler_Capture_sample_rate = FIBER_PROFILER_CAPTURE_SAMPLE_RATE();
	
	Fiber_Profiler_Capture_initialize_options[0] = rb_intern("stall_threshold");
	Fiber_Profiler_Capture_initialize_options[1] = rb_intern("filter_threshold");
	Fiber_Profiler_Capture_initialize_options[2] = rb_intern("track_calls");
	Fiber_Profiler_Capture_initialize_options[3] = rb_intern("sample_rate");
	Fiber_Profiler_Capture_initialize_options[4] = rb_intern("output");
	
	Fiber_Profiler_Capture = rb_define_class_under(Fiber_Profiler, "Capture", rb_cObject);
	rb_define_alloc_func(Fiber_Profiler_Capture, Fiber_Profiler_Capture_allocate);
	
	rb_define_singleton_method(Fiber_Profiler_Capture, "default", Fiber_Profiler_Capture_default, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "initialize", Fiber_Profiler_Capture_initialize, -1);
	
	rb_define_method(Fiber_Profiler_Capture, "start", Fiber_Profiler_Capture_start, 0);
	rb_define_method(Fiber_Profiler_Capture, "stop", Fiber_Profiler_Capture_stop, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "stall_threshold", Fiber_Profiler_Capture_stall_threshold_get, 0);
	rb_define_method(Fiber_Profiler_Capture, "filter_threshold", Fiber_Profiler_Capture_filter_threshold_get, 0);
	rb_define_method(Fiber_Profiler_Capture, "track_calls", Fiber_Profiler_Capture_track_calls_get, 0);
	rb_define_method(Fiber_Profiler_Capture, "sample_rate", Fiber_Profiler_Capture_sample_rate_get, 0);
	
	rb_define_method(Fiber_Profiler_Capture, "stalls", Fiber_Profiler_Capture_stalls_get, 0);
	
	rb_define_singleton_method(Fiber_Profiler_Capture, "default", Fiber_Profiler_Capture_default, 0);
}
