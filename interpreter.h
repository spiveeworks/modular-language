#ifndef MODLANG_INTERPRETER_H
#define MODLANG_INTERPRETER_H

#include "types.h"
/* We really just need the items. We could move them to types.h, or items.h? */
#include "statements.h"

extern bool debug;

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

void shared_buff_decrement(struct shared_buff_header *ptr) {
    if (!ptr) return;

    struct type *elem_type = ptr->element_type;

    ptr->references -= 1;
    if (debug) print_ref_count(ptr);
    if (ptr->references <= 0) {
        if (elem_type->connective == TYPE_ARRAY) {
            uint8 *buff_start = (uint8*)&ptr[1];
            struct shared_buff *arr =
                (struct shared_buff*)(buff_start + ptr->start_offset);
            for (int i = 0; i < ptr->count; i++) {
                shared_buff_decrement(arr[i].ptr);
            }
        } else if (elem_type->connective != TYPE_INT) {
            static bool warned_leak;
            if (!warned_leak) {
                fprintf(stderr, "Warning: Leaking data since aggregate "
                    "garbage collection is not yet implemented.\n");
                warned_leak = true;
            }
        }

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
    if (element_type->connective == TYPE_ARRAY) {
        struct shared_buff *buffs = source;
        for (int i = 0; i < count; i++) {
            struct shared_buff_header *header = buffs[i].ptr;
            if (header) header->references += 1;
            if (debug) {
                print_ref_count(header);
                printf("count is %d\n", buffs[i].count);
            }
        }
    } else if (element_type->connective != TYPE_INT) {
        fprintf(stderr, "Error: Copying aggregate data is not yet implemented.\n");
    }
}

/**********************/
/* Runtime Call Stack */
/**********************/

struct execution_frame {
    struct instruction *start;
    size_t count;
    size_t current;

    size_t locals_start;
    size_t locals_count;
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
    void *pointer;
    uint8 bytes[16];
    struct shared_buff shared_buff;
};

enum variable_memory_mode {
    VARIABLE_UNBOUND,
    VARIABLE_DIRECT_VALUE, /* Do nothing when the variable is unbound. */
    VARIABLE_MEMORY_STACK, /* Free the memory stack back to this variable. */
    VARIABLE_HEAP_OWNED, /* Free this variable from the heap. */
    /* TODO: change this to VARIABLE_SHARED_BUFF? */
    VARIABLE_REFCOUNT /* Decrement this variable's reference count, and free it
                         if the count is zero. */
};

struct variable_data {
    union variable_contents value;
    enum variable_memory_mode mem_mode;
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
/*
struct memory_stack {

};
*/

/* Logically this is a single heterogeneous stack, like the call stack of a CPU
   in virtual memory. In practice we split it across three homogeneous buffers,
   because we have to handle the branches and indexing in software, rather than
   pipelined hardware. */
struct call_stack {
    struct execution_stack exec;
    struct variable_stack vars;
    /* struct memory_stack memory; */
};

void call_stack_push_exec_frame(
    struct call_stack *stack,
    struct instruction_buffer *code
) {
    struct execution_frame *frame = buffer_addn(stack->exec, 1);

    frame->start = code->data;
    frame->count = code->count;
    frame->current = 0;

    frame->locals_start = stack->vars.count;
    frame->locals_count = 0;
}

/* Try to decode the ref, but only crash if it is corrupted, not if it is
   REF_NULL. It isn't the interpreter's job to make sure that REF_NULL is used
   correctly. */
union variable_contents read_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref,
    enum variable_memory_mode *mem_mode
) {
    size_t index = 0;
    switch (ref.type) {
    case REF_NULL:
        if (mem_mode) *mem_mode = VARIABLE_DIRECT_VALUE;
        return (union variable_contents){0};
    case REF_CONSTANT:
        if (mem_mode) *mem_mode = VARIABLE_DIRECT_VALUE;
        return (union variable_contents){.val64 = ref.x};
    case REF_STATIC_POINTER:
        if (mem_mode) *mem_mode = VARIABLE_DIRECT_VALUE;
        return (union variable_contents){.pointer = (void*)ref.x};
    case REF_GLOBAL:
        index = ref.x;
        break;
    case REF_LOCAL:
        index = frame->locals_start + ref.x;
        break;
    case REF_TEMPORARY:
        index = frame->locals_start + frame->locals_count + ref.x;
        break;
    default:
        fprintf(stderr, "Unexpected ref.type value %d?\n", ref.type);
        exit(EXIT_FAILURE);
    }

    if (mem_mode) *mem_mode = vars->data[index].mem_mode;
    return vars->data[index].value;
}

void write_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref,
    union variable_contents value,
    enum variable_memory_mode mem_mode
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
        index = frame->locals_start + ref.x;
        break;
    case REF_TEMPORARY:
        index = frame->locals_start + frame->locals_count + ref.x;
        break;
    default:
        fprintf(stderr, "Unexpected ref.type value %d?\n", ref.type);
        exit(EXIT_FAILURE);
    }
    if (index + 1 > vars->count) buffer_setcount(*vars, index + 1);
    vars->data[index].value = value;
    vars->data[index].mem_mode = mem_mode;
}

void unbind_variable(struct variable_stack *vars, size_t index) {
    if (vars->data[index].mem_mode == VARIABLE_REFCOUNT) {
        shared_buff_decrement(vars->data[index].value.shared_buff.ptr);
    }
    vars->data[index].mem_mode = VARIABLE_UNBOUND;
}

void unbind_temporaries(struct variable_stack *vars) {
    while (vars->count > vars->global_count) {
        unbind_variable(vars, vars->count - 1);
        vars->count -= 1;
    }
}

void continue_execution(struct call_stack *stack) {
    while (stack->exec.count > 0) {
        struct execution_frame *frame = buffer_top(stack->exec);
        if (frame->current < 0 || frame->current >= frame->count) {
            buffer_change_count(stack->exec, -1);
            continue;
        }

        struct instruction *next = &frame->start[frame->current];

        /* Execute instruction. */
        enum variable_memory_mode arg1_mode, arg2_mode;
        /* TODO: refactor this into functions that extract
           scalars/shared_buffs, checking that the mem_mode is correct as they
           go. May require detecting and separating out scalar operations,
           which is something that I am considering anyway. */
        union variable_contents arg1_full =
            read_ref(frame, &stack->vars, next->arg1, &arg1_mode);
        union variable_contents arg2_full =
            read_ref(frame, &stack->vars, next->arg2, &arg2_mode);
        int64 arg1 = arg1_full.val64;
        int64 arg2 = arg2_full.val64;
        union variable_contents result = {0};
        enum variable_memory_mode result_mode = VARIABLE_DIRECT_VALUE;
        struct ref output_ref = next->output;
        switch (next->op) {
        case OP_NULL:
            break;
        case OP_MOV:
            result = arg1_full;
            if (next->flags == OP_SHARED_BUFF) {
                result_mode = VARIABLE_REFCOUNT;
                if (result.shared_buff.ptr != NULL) {
                    if (arg1_mode != VARIABLE_REFCOUNT) {
                        fprintf(stderr, "Error: Tried to move an array but "
                            "the arg was not an array?\n");
                        exit(EXIT_FAILURE);
                    }
                    result.shared_buff.ptr->references += 1;
                    if (debug) {
                        print_ref_count(result.shared_buff.ptr);
                        printf("count is %d\n", result.shared_buff.count);
                    }
                }
            }
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
        case OP_ARRAY_ALLOC:
            result.shared_buff = shared_buff_alloc(arg1_full.pointer, arg2);
            result_mode = VARIABLE_REFCOUNT;
            break;
        case OP_ARRAY_STORE:
          {
            /* TODO: check that the 'output' array is a shared_buff. */
            union variable_contents output =
                read_ref(frame, &stack->vars, next->output, NULL);
            /* TODO: check that the memory accessed is actually an initialised
               and aligned part of the buffer. */
            uint8 *data = shared_buff_get_index(output.shared_buff, arg1);
            struct type *element_type = output.shared_buff.ptr->element_type;
            copy_vals(element_type, data, arg2_full.bytes, 1);
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
                fprintf(stderr, "Error: Arrays of structs are not implemented.\n");
                exit(EXIT_FAILURE);
            }
            copy_vals(element_type, result.bytes, data, 1);
            if (element_type->connective == TYPE_ARRAY) {
                result_mode = VARIABLE_REFCOUNT;
            } else if (element_type->connective != TYPE_INT) {
                fprintf(stderr, "Warning: Array contains types other than "
                    "arrays or ints, runtime may leak it.\n");
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
            result_mode = VARIABLE_REFCOUNT;

            /* TODO: do these need to be checked? Or can we get more lean?
               Inlining may resolve this anyway. */
            uint8 *source_1 = shared_buff_get_index(arg1_full.shared_buff, 0);
            uint8 *dest_1 = shared_buff_get_index(result.shared_buff, 0);
            copy_vals(element_type, dest_1, source_1, arg1_count);

            uint8 *source_2 = shared_buff_get_index(arg2_full.shared_buff, 0);
            uint8 *dest_2 = shared_buff_get_index(result.shared_buff, arg1_count);
            copy_vals(element_type, dest_2, source_2, arg2_count);

            break;
          }
        default:
            fprintf(stderr, "Error: Tried to execute unknown opcode %d.\n",
                next->op);
            exit(EXIT_FAILURE);
        }

        if (next->arg1.type == REF_TEMPORARY) {
            unbind_variable(
                &stack->vars,
                frame->locals_start + frame->locals_count + next->arg1.x
            );
        }
        if (next->arg2.type == REF_TEMPORARY) {
            unbind_variable(
                &stack->vars,
                frame->locals_start + frame->locals_count + next->arg2.x
            );
        }

        write_ref(frame, &stack->vars, output_ref, result, result_mode);

        if (next->output.type == REF_LOCAL
            && next->output.x >= frame->locals_count)
        {
            frame->locals_count = next->output.x + 1;
        } else if (next->output.type == REF_GLOBAL
            && next->output.x >= stack->vars.global_count)
        {
            stack->vars.global_count = next->output.x + 1;
        }

        while (buffer_top(stack->vars)->mem_mode == VARIABLE_UNBOUND) {
            stack->vars.count -= 1;
        }

        frame->current += 1;
    }
}

void execute_top_level_code(
    struct call_stack *stack,
    struct instruction_buffer *statement_code
) {
    if (stack->exec.count != 0) {
        fprintf(stderr, "Error: Tried to execute top-level-code while another "
            "function call was already in process?\n");
        exit(EXIT_FAILURE);
    }

    unbind_temporaries(&stack->vars);
    call_stack_push_exec_frame(stack, statement_code);

    continue_execution(stack);
}

#endif
