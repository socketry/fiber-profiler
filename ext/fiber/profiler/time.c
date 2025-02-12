// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "time.h"

void Fiber_Profiler_Time_elapsed(const struct timespec* start, const struct timespec* stop, struct timespec *duration)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		duration->tv_sec = stop->tv_sec - start->tv_sec - 1;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		duration->tv_sec = stop->tv_sec - start->tv_sec;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
}

float Fiber_Profiler_Time_duration(const struct timespec *duration)
{
	return duration->tv_sec + duration->tv_nsec / 1000000000.0;
}

void Fiber_Profiler_Time_current(struct timespec *time) {
	clock_gettime(CLOCK_MONOTONIC, time);
}

float Fiber_Profiler_Time_proportion(const struct timespec *duration, const struct timespec *total_duration) {
	return Fiber_Profiler_Time_duration(duration) / Fiber_Profiler_Time_duration(total_duration);
}

float Fiber_Profiler_Time_delta(const struct timespec *start, const struct timespec *stop) {
	struct timespec duration;
	Fiber_Profiler_Time_elapsed(start, stop, &duration);
	
	return Fiber_Profiler_Time_duration(&duration);
}
