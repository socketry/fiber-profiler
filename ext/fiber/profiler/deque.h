// Released under the MIT License.
// Copyright, 2023, by Samuel Williams.

// Provides a simple implementation of unique pointers to elements of the given size.

#include <ruby.h>
#include <stdlib.h>
#include <assert.h>

enum {
#ifdef RUBY_DEBUG
	Fiber_Profiler_Deque_DEBUG = 1,
#else
	Fiber_Profiler_Deque_DEBUG = 1,
#endif
};

// A page of elements:
struct Fiber_Profiler_Deque_Page {
	struct Fiber_Profiler_Deque_Page *head, *tail;
	
	size_t size;
	size_t capacity;
	
	char elements[];
};

struct Fiber_Profiler_Deque_Page *Fiber_Profiler_Deque_Page_allocate(size_t element_size, size_t capacity)
{
	struct Fiber_Profiler_Deque_Page *page = (struct Fiber_Profiler_Deque_Page *)malloc(sizeof(struct Fiber_Profiler_Deque_Page) + element_size * capacity);
	
	if (page) {
		page->head = page->tail = NULL;
		
		page->size = 0;
		page->capacity = capacity;
		
		memset(page->elements, 0, element_size * capacity);
	}
	
	return page;
}

inline static void *Fiber_Profiler_Deque_Page_get(struct Fiber_Profiler_Deque_Page *page, size_t index, size_t element_size)
{
#ifdef RUBY_DEBUG
	if (index >= page->size) {
		fprintf(stderr, "Fiber_Profiler_Deque_Page_get: index=%lu, size=%lu\n", index, page->size);
	}
#endif
	
	RUBY_ASSERT(index < page->size);
	
	return (void*)((char *)page->elements + (index * element_size));
}

inline static struct Fiber_Profiler_Deque_Page *Fiber_Profiler_Deque_Page_free(struct Fiber_Profiler_Deque_Page *page, size_t element_size, void (*element_free)(void*))
{
	struct Fiber_Profiler_Deque_Page *tail = page->tail;
	
	if (element_free) {
		for (size_t i = 0; i < page->size; i += 1) {
			void *element = Fiber_Profiler_Deque_Page_get(page, i, element_size);
			element_free(element);
		}
	}
	
	free(page);
	
	return tail;
}

inline static void Fiber_Profiler_Deque_Page_truncate(struct Fiber_Profiler_Deque_Page *page, size_t size, size_t element_size, void (*element_free)(void*))
{
	if (size < page->size) {
		if (element_free) {
			for (size_t i = size; i < page->size; i += 1) {
				void *element = Fiber_Profiler_Deque_Page_get(page, i, element_size);
				element_free(element);
			}
		}
		
		page->size = size;
	}
}

struct Fiber_Profiler_Deque {
	// The deque of elements:
	struct Fiber_Profiler_Deque_Page *head, *tail;
	
	// The current capacity:
	size_t capacity;
	
	// The size of each element that is allocated:
	size_t element_size;
	
	void (*element_initialize)(void*);
	void (*element_free)(void*);
};

inline static int Fiber_Profiler_Deque_initialize(struct Fiber_Profiler_Deque *deque, size_t element_size)
{
	deque->head = deque->tail = NULL;
	
	deque->capacity = 0;
	
	deque->element_size = element_size;
	
	return 0;
}

inline static size_t Fiber_Profiler_Deque_memory_size(const struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->head;
	
	size_t size = 0;
	while (page) {
		size += sizeof(struct Fiber_Profiler_Deque_Page);
		size += page->capacity * deque->element_size;
		page = page->tail;
	}
	
	return size;
}

inline static void Fiber_Profiler_Deque_free(struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->head;
	
	deque->head = deque->tail = NULL;
	
	while (page) {
		page = Fiber_Profiler_Deque_Page_free(page, deque->element_size, deque->element_free);
	}
}

inline static void Fiber_Profiler_Deque_debug(struct Fiber_Profiler_Deque *deque, const char * operation)
{
	struct Fiber_Profiler_Deque_Page *page = deque->head;
	
	while (page) {
		// fprintf(stderr, "Fiber_Profiler_Deque: %s: page=%p, size=%lu, capacity=%lu\n", operation, page, page->size, page->capacity);
		
		RUBY_ASSERT(page->size <= page->capacity);
		
		RUBY_ASSERT(page->head == NULL || page->head->tail == page);
		RUBY_ASSERT(page->tail == NULL || page->tail->head == page);
		
		page = page->tail;
	}
}

void Fiber_Profiler_Deque_truncate(struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->tail;
	
	while (page) {
		Fiber_Profiler_Deque_Page_truncate(page, 0, deque->element_size, deque->element_free);
		
		page = page->head;
	}
	
	deque->tail = deque->head;
	
	if (Fiber_Profiler_Deque_DEBUG) Fiber_Profiler_Deque_debug(deque, __FUNCTION__);
}

inline static size_t Fiber_Profiler_Deque_default_capacity(struct Fiber_Profiler_Deque *deque)
{
	static const size_t target_size = 4096*8;
	
	return (target_size - sizeof(struct Fiber_Profiler_Deque_Page)) / deque->element_size;
}

void *Fiber_Profiler_Deque_push(struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->tail;
	
	if (page == NULL) {
		page = Fiber_Profiler_Deque_Page_allocate(deque->element_size, Fiber_Profiler_Deque_default_capacity(deque));
		
		if (page == NULL) {
			return NULL;
		}
		
		deque->head = deque->tail = page;
		deque->capacity += page->capacity;
	} else if (page->size == page->capacity) {
		if (page->tail) {
			page = page->tail;
			deque->tail = page;
		} else {
			page = Fiber_Profiler_Deque_Page_allocate(deque->element_size, Fiber_Profiler_Deque_default_capacity(deque));
			
			if (page == NULL) {
				return NULL;
			}
			
			page->head = deque->tail;
			deque->tail->tail = page;
			deque->tail = page;
			deque->capacity += page->capacity;
		}
	}
	
	// Push a new element:
	page->size += 1;
	void *element = Fiber_Profiler_Deque_Page_get(page, page->size - 1, deque->element_size);
	
	if (deque->element_initialize) {
		deque->element_initialize(element);
	}
	
	if (Fiber_Profiler_Deque_DEBUG) Fiber_Profiler_Deque_debug(deque, __FUNCTION__);
	
	return element;
}

inline static void *Fiber_Profiler_Deque_pop(struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->tail;
	
	if (page == NULL) {
		return NULL;
	}
	
	while (page->size == 0) {
		if (page->head) {
			page = page->head;
		} else {
			return NULL;
		}
	}
	
	deque->tail = page;
	
	// Pop the last element:
	void *element = Fiber_Profiler_Deque_Page_get(page, page->size - 1, deque->element_size);
	page->size -= 1;
	
	if (deque->element_free) {
		deque->element_free(element);
	}
	
	if (Fiber_Profiler_Deque_DEBUG) Fiber_Profiler_Deque_debug(deque, __FUNCTION__);
	
	return element;
}

inline static void *Fiber_Profiler_Deque_last(struct Fiber_Profiler_Deque *deque)
{
	struct Fiber_Profiler_Deque_Page *page = deque->tail;
	
	if (page == NULL) {
		return NULL;
	}
	
	while (page->size == 0) {
		if (page->head) {
			page = page->head;
		} else {
			return NULL;
		}
	}
	
	return Fiber_Profiler_Deque_Page_get(page, page->size - 1, deque->element_size);
}

#define Fiber_Profiler_Deque_each(deque, type, element) \
	for (struct Fiber_Profiler_Deque_Page *page = (deque)->head; page != NULL && page->size; page = page->tail) \
		for (size_t i = 0; i < page->size; i++) \
			for (type *element = Fiber_Profiler_Deque_Page_get(page, i, sizeof(type)); element != NULL; element = NULL)
