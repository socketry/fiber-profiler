// Released under the MIT License.
// Copyright, 2023, by Samuel Williams.

// Provides a simple implementation of unique pointers to elements of the given size.

#include <ruby.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

struct Fiber_Profiler_Array {
	// The array of elements:
	void *base;
	
	// The allocated size of the array in elements
	size_t count;
	
	// The biggest element index that has been used:
	size_t limit;
	
	// The size of each element that is allocated:
	size_t element_size;
	
	void (*element_initialize)(void*);
	void (*element_free)(void*);
};

inline static int Fiber_Profiler_Array_initialize(struct Fiber_Profiler_Array *array, size_t element_size)
{
	array->base = NULL;
	array->count = 0;
	
	array->limit = 0;
	array->element_size = element_size;
	
	return 0;
}

inline static size_t Fiber_Profiler_Array_memory_size(const struct Fiber_Profiler_Array *array)
{
	// Upper bound.
	return array->count * array->element_size;
}

void *Fiber_Profiler_Array_get(void *base, size_t index, size_t element_size)
{
	return (void*)((char *)base + (index * element_size));
}

inline static void Fiber_Profiler_Array_free(struct Fiber_Profiler_Array *array)
{
	if (array->base) {
		void *base = array->base;
		size_t limit = array->limit;
		
		array->base = NULL;
		array->count = 0;
		array->limit = 0;
		
		if (array->element_free) {
			for (size_t i = 0; i < limit; i += 1) {
				void *element = Fiber_Profiler_Array_get(base, i, array->element_size);
				array->element_free(element);
			}
		}
		
		free(base);
	}
}

inline static int Fiber_Profiler_Array_resize(struct Fiber_Profiler_Array *array, size_t count)
{
	if (count <= array->count) {
		// Already big enough:
		return 0;
	}
	
	size_t maximum_count = UINTPTR_MAX / array->element_size;
	
	if (count > maximum_count) {
		errno = ENOMEM;
		return -1;
	}
	
	size_t new_count = array->count;
	
	// If the array is empty, we need to set the initial size:
	if (new_count == 0) {
		size_t page_size = sysconf(_SC_PAGESIZE);
		while (page_size < array->element_size) page_size *= 2;
		new_count = page_size / array->element_size;
	}
	
	else while (new_count < count) {
		// Ensure we don't overflow:
		if (new_count > (maximum_count / 2)) {
			new_count = maximum_count;
			break;
		}
		
		// Compute the next multiple (ideally a power of 2):
		new_count *= 2;
	}
	
	void **new_base = (void**)realloc(array->base, new_count * array->element_size);
	
	if (new_base == NULL) {
		return -1;
	}
	
	// Zero out the new memory:
	memset((void*)((char*)new_base + array->count * array->element_size), 0, (new_count - array->count) * array->element_size);
	
	array->base = (void**)new_base;
	array->count = new_count;
	
	// Resizing successful:
	return 1;
}

void Fiber_Profiler_Array_truncate(struct Fiber_Profiler_Array *array, size_t count)
{
	if (count < array->limit) {
		if (array->element_free) {
			for (size_t i = count; i < array->limit; i += 1) {
				void *element = Fiber_Profiler_Array_get(array->base, i, array->element_size);
				if (element) {
					array->element_free(element);
				}
			}
		}
		
		array->limit = count;
	}
}

void *Fiber_Profiler_Array_lookup(struct Fiber_Profiler_Array *array, size_t index)
{
	if (index < array->limit) {
		return Fiber_Profiler_Array_get(array->base, index, array->element_size);
	} else {
		return NULL;
	}
}

void *Fiber_Profiler_Array_push(struct Fiber_Profiler_Array *array)
{
	if (array->limit == array->count) {
		int result = Fiber_Profiler_Array_resize(array, array->count + 1);
		
		if (result < 0) {
			return NULL;
		}
	}
	
	void *element = Fiber_Profiler_Array_get(array->base, array->limit, array->element_size);
	
	if (array->element_initialize) {
		array->element_initialize(element);
	}
	
	array->limit += 1;
	
	return element;
}

void *Fiber_Profiler_Array_pop(struct Fiber_Profiler_Array *array)
{
	if (array->limit > 0) {
		array->limit -= 1;
		
		void *element = Fiber_Profiler_Array_get(array->base, array->limit, array->element_size);
		
		if (array->element_free) {
			array->element_free(element);
		}
		
		return element;
	} else {
		return NULL;
	}
}

void *Fiber_Profiler_Array_last(struct Fiber_Profiler_Array *array)
{
	if (array->limit > 0) {
		return Fiber_Profiler_Array_get(array->base, array->limit - 1, array->element_size);
	} else {
		return NULL;
	}
}
