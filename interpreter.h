#ifndef MODLANG_INTERPRETER_H
#define MODLANG_INTERPRETER_H

#include "types.h"
/* We really just need the items. We could move them to types.h, or items.h? */
#include "statements.h"

extern bool debug;

/*************************/
/* Procedure Definitions */
/*************************/

struct procedure {
    struct instruction_buffer instructions;
};

struct procedure_buffer {
    struct procedure *data;
    size_t count;
    size_t capacity;
};

/******************/
/* Shared Buffers */
/******************/

/* The functional part of this language is designed around imperative
   manipulations to local copies of complex arrays and so on. This means
   reference-counting is a very natural fit; local copies can be created on
   demand, but in situations where an array is only read and not written to, no
   copying is necessary, including finer granularities where an array of arrays
   is rearranged, but the inner arrays are not modified. */

struct shared_buff_header {
    struct type *element_type;
    int32 references;
    int32 start_offset; /* In bytes. */
    int32 count;
    int32 buffer_size; /* In bytes; this is not a capacity count. */
};

struct shared_buff {
    struct shared_buff_header *ptr;
    int32 start_offset; /* In bytes. */
    int32 count;
};

void print_ref_count(struct shared_buff_header *header) {
    if (header) {
        printf("ref count at %p is now %d\n", header, header->references);
    }
}

/* Allocate a shared buffer big enough to hold count elements each of the
   specified size. */
struct shared_buff shared_buff_alloc(struct type *elem_type, int count) {
    int elem_size = elem_type->total_size;
    struct shared_buff_header *ptr =
        malloc(sizeof(struct shared_buff_header) + elem_size * count);
    ptr->element_type = elem_type;
    ptr->references = 1;
    ptr->start_offset = 0;
    ptr->count = count;
    ptr->buffer_size = elem_size * count;
    if (debug) {
        print_ref_count(ptr);
        printf("count is %d\n", count);
    }

    struct shared_buff result = {ptr, 0, count};
    return result;
}

void shared_buff_decrement(struct shared_buff_header *ptr);

/* Decrements the elements of an array of a known datatype. Passes through the
   whole array multiple times, once for each offset where an array sits. This
   performs best for structs with no arrays in them or just one array in them,
   but probably still does well with two or three arrays. */
void do_decrements(uint8 *data, struct type *type, int count, size_t stride) {
    if (type->connective == TYPE_ARRAY) {
        for (int i = 0; i < count; i++) {
            struct shared_buff *buff = (struct shared_buff*)data;
            shared_buff_decrement(buff->ptr);
            data += stride;
        }
    } else if (type->connective == TYPE_TUPLE) {
        for (int i = 0; i < type->elements.count; i++) {
            struct type *elem_type = &type->elements.data[i];
            do_decrements(data, elem_type, count, stride);
            data += elem_type->total_size;
        }
    } else if (type->connective == TYPE_RECORD) {
        for (int i = 0; i < type->fields.count; i++) {
            struct type *elem_type = &type->fields.data[i].type;
            do_decrements(data, elem_type, count, stride);
            data += elem_type->total_size;
        }
    } else if (type->connective != TYPE_INT) {
        fprintf(stderr, "Warning: Got an unknown type connective, leaking.\n");
    }
}

/* Copy of do_decrements that increments instead. I think this will work. */
void do_increments(uint8 *data, struct type *type, int count, size_t stride) {
    if (type->connective == TYPE_ARRAY) {
        struct shared_buff *buffs = (struct shared_buff*)data;
        for (int i = 0; i < count; i++) {
            struct shared_buff_header *header = buffs[i].ptr;
            if (header) header->references += 1;
            if (debug) {
                print_ref_count(header);
                printf("count is %d\n", buffs[i].count);
            }
        }
    } else if (type->connective == TYPE_TUPLE) {
        for (int i = 0; i < type->elements.count; i++) {
            struct type *elem_type = &type->elements.data[i];
            do_increments(data, elem_type, count, stride);
            data += elem_type->total_size;
        }
    } else if (type->connective == TYPE_RECORD) {
        for (int i = 0; i < type->fields.count; i++) {
            struct type *elem_type = &type->fields.data[i].type;
            do_increments(data, elem_type, count, stride);
            data += elem_type->total_size;
        }
    } else if (type->connective != TYPE_INT) {
        fprintf(stderr, "Warning: Got an unknown type connective, leaking.\n");
    }
}

void shared_buff_decrement(struct shared_buff_header *ptr) {
    if (!ptr) return;

    struct type *elem_type = ptr->element_type;

    ptr->references -= 1;
    if (debug) print_ref_count(ptr);
    if (ptr->references <= 0) {
        uint8 *buff_start = (uint8*)&ptr[1];
        uint8 *data = buff_start + ptr->start_offset;
        do_decrements(data, elem_type, ptr->count, elem_type->total_size);

        free(ptr);
    }
}

void *shared_buff_get_index(struct shared_buff buff, int index) {
    if (index < 0 || index >= buff.count) {
        fprintf(stderr, "Runtime error: Tried to access index %lld of an "
            "array of size %d.\n", (long long)index, buff.count);
        exit(EXIT_FAILURE);
    }
    struct type *element_type = buff.ptr->element_type;
    uint8 *data = (uint8*)&buff.ptr[1];
    data += buff.start_offset;
    data += element_type->total_size * index;
    return data;
}

void copy_vals(struct type *element_type, void *dest, void *source, int count) {
    memcpy(dest, source, count * element_type->total_size);
    do_increments(source, element_type, count, element_type->total_size);
}

void shared_buff_make_unique(struct shared_buff *buff) {
    struct shared_buff_header *ptr = buff->ptr;
    if (ptr->references > 1) {
        struct shared_buff unique = shared_buff_alloc(ptr->element_type, buff->count);

        uint8 *source = shared_buff_get_index(*buff, 0);
        uint8 *dest = shared_buff_get_index(unique, 0);
        copy_vals(ptr->element_type, dest, source, buff->count);

        *buff = unique;
        ptr->references -= 1;
    }
}

void copy_scalar(
    uint8 *dest,
    uint8 *src,
    enum operation_flags flags,
    bool temporary
) {
    if (flags == OP_SHARED_BUFF) {
        struct shared_buff *src_buff = (struct shared_buff*)src;
        struct shared_buff *dest_buff = (struct shared_buff*)dest;
        *dest_buff = *src_buff;
        if (!temporary && src_buff->ptr != NULL) {
            src_buff->ptr->references += 1;
            if (debug) {
                print_ref_count(src_buff->ptr);
                printf("count is %d\n", src_buff->count);
            }
        }
    } else {
        int64 *src64 = (int64*)src;
        int64 *dest64 = (int64*)dest;
        *dest64 = *src64;
    }
}


/**********************/
/* Runtime Call Stack */
/**********************/

struct execution_frame {
    struct instruction *start;
    size_t count;
    size_t current;

    /* Stack offset where function args/locals start for this frame. */
    size_t locals_start;
    /* Stack offset where function results should be written to. Usually this
       is equal to locals_start. */
    size_t results_start;
};

/* Tracks bytecode indices for calling and returning between functions, and
   where in the variable stack each point of execution was indexing from. */
struct execution_stack {
    struct execution_frame *data;
    size_t count;
    size_t capacity;
};

union variable_contents {
    uint64 val64;
    uint8 *pointer;
    uint8 bytes[16];
    struct shared_buff shared_buff;
};

/* TODO: Remove this struct. */
struct variable_data {
    union variable_contents value;
};

/* Stores the actual value or memory location of each variable in use by the
   execution stack. */
struct variable_stack {
    struct variable_data *data;
    size_t count;
    size_t capacity;

    size_t global_count;
};

/* Stores fixed-size structures too big to fit in the variable stack, but too
   small to be worth sharing, or maybe even arrays that have been analysed and
   happen to be used in a LIFO way anyway. */
/* TODO: Get the stack allocator over from the gizmo headers? We might not want
   all of the baggage of allocator interfaces and mem.h and serialization and
   so on, though... */
struct data_stack {
    uint8 *data;
    size_t allocated_count;
    size_t size; /* Don't call it capacity, since we don't actually want to use
                    buffer macros on it. */
};

struct data_stack stack_create(size_t size) {
    struct data_stack result;
    result.data = malloc(size);
    result.allocated_count = 0;
    result.size = size;

    return result;
}

uint8 *stack_alloc(struct data_stack *stack, size_t count) {
    if (stack->allocated_count + count > stack->size) {
        fprintf(stderr, "Error: Ran out of memory in the data stack.\n");
        exit(EXIT_FAILURE);
    }

    uint8 *result = stack->data + stack->allocated_count;
    stack->allocated_count += count;

    return result;
}

void stack_free(struct data_stack *stack, uint8 *ptr) {
    if (ptr < stack->data || ptr > stack->data + stack->size) {
        fprintf(stderr, "Warning: Tried to free memory location %llp from the "
            "stack, but it wasn't on the stack.\n", (uint64)ptr);
        return;
    }

    if (ptr > stack->data + stack->allocated_count) {
        fprintf(stderr, "Warning: Tried to free memory location %llp, but it "
            "was already free.\n", (uint64)ptr);
        return;
    }

    stack->allocated_count = ptr - stack->data;
}

/* Logically this is a single heterogeneous stack, like the call stack of a CPU
   in virtual memory. In practice we split it across three homogeneous buffers,
   because we have to handle the branches and indexing in software, rather than
   pipelined hardware. */
struct call_stack {
    struct execution_stack exec;
    struct variable_stack vars;
    struct data_stack data;
};

void call_stack_push_exec_frame(
    struct call_stack *stack,
    struct instruction_buffer *code
) {
    struct execution_frame *frame = buffer_addn(stack->exec, 1);

    frame->start = code->data;
    frame->count = code->count;
    frame->current = 0;

    frame->locals_start = stack->vars.global_count;
    frame->results_start = stack->vars.count;
}

/* Try to decode the ref, but only crash if it is corrupted, not if it is
   REF_NULL. It isn't the interpreter's job to make sure that REF_NULL is used
   correctly. */
union variable_contents read_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref
) {
    size_t index = 0;
    switch (ref.type) {
    case REF_NULL:
        return (union variable_contents){0};
    case REF_CONSTANT:
        return (union variable_contents){.val64 = ref.x};
    case REF_STATIC_POINTER:
        return (union variable_contents){.pointer = (void*)ref.x};
    case REF_GLOBAL:
        index = ref.x;
        break;
    case REF_LOCAL:
    case REF_TEMPORARY:
        index = frame->locals_start + ref.x;
        break;
    default:
        fprintf(stderr, "Unexpected ref.type value %d?\n", ref.type);
        exit(EXIT_FAILURE);
    }

    return vars->data[index].value;
}

void write_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref,
    union variable_contents value
) {
    size_t index = 0;
    switch (ref.type) {
    case REF_NULL:
        /* Some operations are just used for their side-effects. */
        return;
    case REF_CONSTANT:
        fprintf(stderr, "Error: tried to write value to a constant?\n");
        exit(EXIT_FAILURE);
    case REF_GLOBAL:
        index = ref.x;
        break;
    case REF_LOCAL:
    case REF_TEMPORARY:
        index = frame->locals_start + ref.x;
        break;
    default:
        fprintf(stderr, "Unexpected ref.type value %d?\n", ref.type);
        exit(EXIT_FAILURE);
    }
    if (index + 1 > vars->count) buffer_setcount(*vars, index + 1);
    vars->data[index].value = value;
}

void continue_execution(
    struct procedure_buffer procedures,
    struct call_stack *stack
) {
    while (stack->exec.count > 0) {
        struct execution_frame *frame = buffer_top(stack->exec);
        if (frame->current < 0 || frame->current >= frame->count) {
            stack->exec.count -= 1;
            continue;
        }

        struct instruction *next = &frame->start[frame->current];

        /* Execute instruction. */
        union variable_contents arg1_full =
            read_ref(frame, &stack->vars, next->arg1);
        union variable_contents arg2_full =
            read_ref(frame, &stack->vars, next->arg2);
        int64 arg1 = arg1_full.val64;
        int64 arg2 = arg2_full.val64;
        union variable_contents result = {0};
        struct ref output_ref = next->output;
        switch (next->op) {
        case OP_NULL:
            break;
        case OP_MOV:
            copy_scalar(
                result.bytes,
                arg1_full.bytes,
                next->flags,
                next->arg1.type == REF_TEMPORARY
            );
            break;
        case OP_LOR:
            result.val64 = arg1 || arg2;
            break;
        case OP_LAND:
            result.val64 = arg1 && arg2;
            break;
        case OP_EQ:
            result.val64 = arg1 == arg2;
            break;
        case OP_NEQ:
            result.val64 = arg1 != arg2;
            break;
        case OP_LEQ:
            result.val64 = arg1 <= arg2;
            break;
        case OP_GEQ:
            result.val64 = arg1 >= arg2;
            break;
        case OP_LESS:
            result.val64 = arg1 < arg2;
            break;
        case OP_GREATER:
            result.val64 = arg1 > arg2;
            break;
        case OP_BOR:
            result.val64 = arg1 | arg2;
            break;
        case OP_BAND:
            result.val64 = arg1 & arg2;
            break;
        case OP_BXOR:
            result.val64 = arg1 ^ arg2;
            break;
        case OP_PLUS:
            result.val64 = arg1 + arg2;
            break;
        case OP_MINUS:
            result.val64 = arg1 - arg2;
            break;
        case OP_LSHIFT:
            result.val64 = arg1 << arg2;
            break;
        case OP_RSHIFT:
            result.val64 = arg1 >> arg2;
            break;
        case OP_MUL:
            result.val64 = arg1 * arg2;
            break;
        case OP_DIV:
            result.val64 = arg1 / arg2;
            break;
        case OP_MOD:
            result.val64 = arg1 % arg2;
            break;
        case OP_EDIV:
            if (arg1 >= 0) result.val64 = arg1 / arg2;
            else result.val64 = (arg1 - arg2 + 1) / arg2;
            break;
        case OP_EMOD:
            /* The naive algorithm in the negative case is
               `arg2 - ((-arg1) % arg2)`; in modular arithmetic this is
               equivalent to arg1 % arg2, but by making arg1 positive before
               computing the modulo, we can know that the result will actually
               be positive at the end. */
            /* The problem with this, though, is that ((-arg1) % arg2) is in
               the range [0, arg2 - 1], so arg2 - that is in the range
               [1, arg2], when we wanted it to stay in the range [0, arg2 - 1].
               By subtracting 1 we at least get an operation that maps
               [0, arg2 - 1] to itself, but then we need to correct by
               computing (-arg1 - 1) % arg2 instead; these two shifts by -1
               cancel out when one is subtracted from the other.
                 e.g. arg1=-arg2 would give (-arg1 - 1) % arg2 = arg2 - 1,
                      then arg2 - 1 - (arg2 - 1) = 0, which is what we want,
                 and yet arg1=-1 would give (-arg1 - 1) % arg2 = 0,
                      then arg2 - 1 - 0 = arg2 - 1, which is also correct. */
            if (arg1 >= 0) result.val64 = arg1 % arg2;
            else result.val64 = arg2 - 1 - (-arg1 - 1) % arg2;
            break;
        case OP_CALL:
        {
            struct execution_frame new;
            new.start = procedures.data[arg1].instructions.data;
            new.count = procedures.data[arg1].instructions.count;
            new.current = 0;
            new.locals_start = frame->locals_start + arg2;
            if (next->arg1.type == REF_TEMPORARY) {
                new.results_start = new.locals_start - 1;
            } else {
                new.results_start = new.locals_start;
            }

            buffer_push(stack->exec, new);

            break;
        }
        case OP_RET:
        {
            /* Unbind all variables that aren't being returned. */
            int source_offset = frame->locals_start + arg1;
            int dest_offset = frame->results_start;
            /* Move results up the stack, to where the inputs were. */
            for (int i = 0; i < arg2; i++) {
                stack->vars.data[dest_offset + i] =
                    stack->vars.data[source_offset + i];
            }

            stack->exec.count -= 1;

            break;
        }
        case OP_ARRAY_ALLOC:
            result.shared_buff = shared_buff_alloc((struct type *)arg1_full.pointer, arg2);
            break;
        case OP_ARRAY_OFFSET:
            result.pointer = shared_buff_get_index(arg1_full.shared_buff, arg2);
            break;
        case OP_ARRAY_OFFSET_MAKE_UNIQUE:
          {
            /* Make the variable on the stack unique. */
            shared_buff_make_unique(&arg1_full.shared_buff);
            write_ref(frame, &stack->vars, next->arg1, arg1_full);

            /* Then do the offset operation to the unique version. */
            result.pointer = shared_buff_get_index(arg1_full.shared_buff, arg2);
            break;
          }
        case OP_ARRAY_STORE:
          {
            /* TODO: check that the 'output' array is a shared_buff. */
            union variable_contents output =
                read_ref(frame, &stack->vars, output_ref);
            /* TODO: check that the memory accessed is actually an initialised
               and aligned part of the buffer. */
            uint8 *data = shared_buff_get_index(output.shared_buff, arg1);
            struct type *element_type = output.shared_buff.ptr->element_type;
            copy_scalar(
                data,
                arg2_full.bytes,
                next->flags,
                next->arg2.type == REF_TEMPORARY
            );
            /* Stop the array variable from being overwritten. */
            output_ref.type = REF_NULL;
            break;
          }
        case OP_ARRAY_INDEX:
          {
            /* TODO: check that the memory accessed is actually an initialised
               and aligned part of the buffer. */
            uint8 *data = shared_buff_get_index(arg1_full.shared_buff, arg2);
            struct type *element_type = arg1_full.shared_buff.ptr->element_type;
            if (element_type->total_size > 16) {
                fprintf(stderr, "Error: Tried to read a scalar from an array of structs.\n");
                exit(EXIT_FAILURE);
            }
            copy_scalar(result.bytes, data, next->flags, false);
            if (next->output.type == next->arg1.type && next->output.x == next->arg1.x) {
                /* If we are overwriting the array, then decrement it first. */
                shared_buff_decrement(arg1_full.shared_buff.ptr);
            }
            break;
          }
        case OP_ARRAY_CONCAT:
          {
            /* TODO: check that the two arrays have the same type? Is this
               guaranteed? */
            struct type *element_type = arg1_full.shared_buff.ptr->element_type;

            int arg1_count = arg1_full.shared_buff.count;
            int arg2_count = arg2_full.shared_buff.count;
            int result_count = arg1_count + arg2_count;
            result.shared_buff = shared_buff_alloc(element_type, result_count);

            /* TODO: do these need to be checked? Or can we get more lean?
               Inlining may resolve this anyway. */
            uint8 *source_1 = shared_buff_get_index(arg1_full.shared_buff, 0);
            uint8 *dest_1 = shared_buff_get_index(result.shared_buff, 0);
            copy_vals(element_type, dest_1, source_1, arg1_count);

            uint8 *source_2 = shared_buff_get_index(arg2_full.shared_buff, 0);
            uint8 *dest_2 = shared_buff_get_index(result.shared_buff, arg1_count);
            copy_vals(element_type, dest_2, source_2, arg2_count);

            if (next->arg1.type == REF_TEMPORARY) {
                shared_buff_decrement(arg1_full.shared_buff.ptr);
            }
            if (next->arg2.type == REF_TEMPORARY) {
                shared_buff_decrement(arg2_full.shared_buff.ptr);
            }

            break;
          }
        case OP_DECREMENT_REFCOUNT:
            shared_buff_decrement(arg1_full.shared_buff.ptr);
            break;
        case OP_STACK_ALLOC:
            result.pointer = stack_alloc(&stack->data, arg1);
            break;
        case OP_STACK_FREE:
            stack_free(&stack->data, arg1_full.pointer);
            break;
        case OP_POINTER_OFFSET:
            result.pointer = arg1_full.pointer + arg2;
            break;
        case OP_POINTER_STORE:
          {
            /* TODO: check that the 'output' array is a shared_buff. */
            union variable_contents output =
                read_ref(frame, &stack->vars, output_ref);
            /* TODO: check that the memory accessed is actually an initialised
               and aligned part of the buffer. */
            void *data = output.pointer + arg1;
            copy_scalar(
                data,
                arg2_full.bytes,
                next->flags,
                next->arg2.type == REF_TEMPORARY
            );
            /* Stop the struct variable from being overwritten. */
            output_ref.type = REF_NULL;
            break;
          }
        case OP_POINTER_COPY:
          {
            union variable_contents output =
                read_ref(frame, &stack->vars, output_ref);
            memcpy(output.pointer, arg1_full.pointer, arg2);
            /* Stop the struct variable from being overwritten. */
            output_ref.type = REF_NULL;
            break;
          }
        case OP_POINTER_DUP:
          {
            result.pointer = stack_alloc(&stack->data, arg2);
            memcpy(result.pointer, arg1_full.pointer, arg2);
            break;
          }
        case OP_POINTER_COPY_OVERLAPPING:
          {
            union variable_contents output =
                read_ref(frame, &stack->vars, output_ref);
            memmove(output.pointer, arg1_full.pointer, arg2);
            /* Stop the struct variable from being overwritten. */
            output_ref.type = REF_NULL;
            break;
          }
        case OP_POINTER_LOAD:
          {
            void *data = arg1_full.pointer + arg2;
            copy_scalar(result.bytes, data, next->flags, false);
            break;
          }
        case OP_POINTER_LOAD_MAKE_UNIQUE:
          {
            /* Get the pointer to the array. */
            void *addr = arg1_full.pointer + arg2;
            struct shared_buff *data = addr;

            /* Make it unique. */
            shared_buff_make_unique(data);

            /* Now put it somewhere. */
            /* We don't increment it, because this opcode is only for
               temporaries on the LHS. We could introduce a runtime construct,
               and possibly even a frontend type, representing a mutable array
               view, but for now we just use an array that secretly hasn't had
               its refcount increased. */
            result.shared_buff = *data;
            break;
          }
        case OP_POINTER_INCREMENT_REFCOUNT:
          {
              void *data = arg1_full.pointer + arg2;
              struct shared_buff *buff = (struct shared_buff*)data;
              if (buff->ptr) {
                  buff->ptr->references += 1;
              }
              if (debug) {
                  print_ref_count(buff->ptr);
                  printf("count is %d\n", buff->count);
              }
              break;
          }
        case OP_POINTER_DECREMENT_REFCOUNT:
          {
              void *data = arg1_full.pointer + arg2;
              struct shared_buff *buff = (struct shared_buff*)data;
              shared_buff_decrement(buff->ptr);
              break;
          }
        default:
            fprintf(stderr, "Error: Tried to execute unknown opcode %d.\n",
                next->op);
            exit(EXIT_FAILURE);
        }

        write_ref(frame, &stack->vars, output_ref, result);

        if (next->output.type == REF_GLOBAL
            && next->output.x >= stack->vars.global_count)
        {
            stack->vars.global_count = next->output.x + 1;
        }

        /* We may have just returned, in which case this change is allowed, but
           discarded. We also may have just called into something, in which
           case this change is allowed, and necessary, in order to return to
           one past the call site. */
        frame->current += 1;
    }
}

void execute_top_level_code(
    struct procedure_buffer procedures,
    struct call_stack *stack,
    struct instruction_buffer *statement_code
) {
    if (stack->exec.count != 0) {
        fprintf(stderr, "Error: Tried to execute top-level-code while another "
            "function call was already in process?\n");
        exit(EXIT_FAILURE);
    }

    call_stack_push_exec_frame(stack, statement_code);

    continue_execution(procedures, stack);
}

#endif
