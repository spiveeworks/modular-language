#ifndef MODLANG_BUFFER_H
#define MODLANG_BUFFER_H

#include <stdlib.h>

#define buffer_setcap(A, N) ((A).data = array_realloc_proc((A).data, sizeof(*(A).data), (A).capacity, (N)), (A).capacity = (N), (A).data)
#define buffer_grow(A, N) (buffer_grow_proc((void**)&(A).data, sizeof(*(A).data), &(A).capacity, (N)))

#define buffer_reserve(A, N) ((A).capacity < (N) ? buffer_setcap((A), (N)) : 0)
#define buffer_maybe_grow(A, N) ((A).capacity < (A).count + (N) ? buffer_grow((A), (N)) : 0)

#define buffer_change_count(A, N) (buffer_maybe_grow((A), (N)), \
    (A).count += (N))
#define buffer_addn(A, N) (buffer_change_count((A), (N)), &(A).data[(A).count - (N)])
#define buffer_push(A, X) ((*buffer_addn((A), 1) = (X)))

#define buffer_setcount(A, N) (buffer_reserve((A), (N)), (A).count = (N))

#define buffer_top(A) ((A).count > 0 ? &(A).data[(A).count - 1] : NULL)
#define buffer_free(A) ((A).capacity > 0 ? free((A).data) : 0)
#define buffer_pop(A) ((A).data[--(A).count])

void *array_realloc_proc(
    void *data,
    size_t size,
    size_t capacity,
    size_t new_capacity
) {
    if (new_capacity > 0) {
        if (capacity > 0) return realloc(data, size * new_capacity);
        /* else */ return malloc(size * new_capacity);
    } else {
        if (capacity > 0) free(data);
        return NULL;
    }
}

void buffer_grow_proc(
    void **data,
    size_t size,
    size_t *capacity,
    size_t grow_by
) {
    size_t new_capacity = *capacity + grow_by;
    if (new_capacity < 2 * (*capacity)) {
        new_capacity = 2 * (*capacity);
    }
    if (new_capacity < 4) {
        new_capacity = 4;
    }
    *data = array_realloc_proc(*data, size, *capacity, new_capacity);
    *capacity = new_capacity;
}

#endif
