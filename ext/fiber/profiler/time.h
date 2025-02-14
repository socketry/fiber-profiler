// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include <time.h>

void Fiber_Profiler_Time_elapsed(const struct timespec* start, const struct timespec* stop, struct timespec *duration);

static inline double Fiber_Profiler_Time_duration(const struct timespec *duration)
{
	return duration->tv_sec + duration->tv_nsec / 1000000000.0;
}

static inline double Fiber_Profiler_Time_proportion(const struct timespec *duration, const struct timespec *total_duration)
{
	return Fiber_Profiler_Time_duration(duration) / Fiber_Profiler_Time_duration(total_duration);
}

static inline void Fiber_Profiler_Time_current(struct timespec *time)
{
	clock_gettime(CLOCK_MONOTONIC, time);
}

static inline double Fiber_Profiler_Time_delta(const struct timespec *start, const struct timespec *stop)
{
	return stop->tv_sec - start->tv_sec + (stop->tv_nsec - start->tv_nsec) / 1e9;
}

static inline double Fiber_Profiler_Time_delta_current(const struct timespec *start)
{
	struct timespec stop;
	Fiber_Profiler_Time_current(&stop);
	
	return Fiber_Profiler_Time_delta(start, &stop);
}

#define Fiber_Profiler_TIME_PRINTF_TIMESPEC "%.3g"
#define Fiber_Profiler_TIME_PRINTF_TIMESPEC_ARGUMENTS(ts) ((double)(ts).tv_sec + (ts).tv_nsec / 1e9)
