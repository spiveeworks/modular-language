#ifndef MODLANG_BUFFER_H
#define MODLANG_BUFFER_H

#include <stdlib.h>

#define buffer_setcap(A, N) ((A).data = array_realloc_proc((void*)(A).data, sizeof(*(A).data), (size_t)(A).capacity, (size_t)(N), __FILE__, __LINE__), (A).capacity = (N), (A).data)
#define buffer_grow(A, N) ((A).capacity = buffer_grow_proc((void**)&(A).data, sizeof(*(A).data), (size_t)(A).capacity, (size_t)(N), __FILE__, __LINE__))

#define buffer_reserve(A, N) ((A).capacity < (N) ? buffer_setcap((A), (N)) : 0)
#define buffer_maybe_grow(A, N) ((A).capacity < (A).count + (N) ? buffer_grow((A), (N)) : 0)

#define buffer_change_count(A, N) (buffer_maybe_grow((A), (N)), \
    (A).count += (N))
#define buffer_addn(A, N) (buffer_change_count((A), (N)), &(A).data[(A).count - (N)])
#define buffer_push(A, X) ((void)(*buffer_addn((A), 1) = (X)))

#define buffer_setcount(A, N) (buffer_reserve((A), (N)), (A).count = (N))

#define buffer_top(A) ((A).count > 0 ? &(A).data[(A).count - 1] : NULL)
#define buffer_free(A) ((A).capacity > 0 ? free((A).data) : 0)
#define buffer_pop(A) ((A).data[--(A).count])

#ifdef _WIN32
void *malloc_dbg(size_t size, char *filename, int lineno) {
    return _malloc_dbg(size, _NORMAL_BLOCK, filename, lineno);
}

void *realloc_dbg(void *data, size_t size, char *filename, int lineno) {
    return _realloc_dbg(data, size, _NORMAL_BLOCK, filename, lineno);
}
#else
void *malloc_dbg(size_t size, char *filename, int lineno) {
    return malloc(size);
}

void *realloc_dbg(void *data, size_t size, char *filename, int lineno) {
    return realloc(data, size);
}
#endif

void *array_realloc_proc(
    void *data,
    size_t size,
    size_t capacity,
    size_t new_capacity,
    char *filename,
    int lineno
) {
    if (new_capacity > 0) {
        if (capacity > 0) {
            void *new_data = realloc_dbg(data, size * new_capacity, filename, lineno);
            return new_data;
        }
        /* else */ return malloc_dbg(size * new_capacity, filename, lineno);
    } else {
        if (capacity > 0) free(data);
        return NULL;
    }
}

size_t buffer_grow_proc(
    void **data,
    size_t size,
    size_t capacity,
    size_t grow_by_min,
    char *filename,
    int lineno
) {
    size_t new_capacity = capacity + grow_by_min;
    if (new_capacity < 2 * capacity) {
        new_capacity = 2 * capacity;
    }
    if (new_capacity < 4) {
        new_capacity = 4;
    }
    *data = array_realloc_proc(*data, size, capacity, new_capacity, filename, lineno);
    return new_capacity;
}

#endif
