#ifndef MODLANG_INTERPRETER_H
#define MODLANG_INTERPRETER_H

#include "types.h"
/* We really just need the items. We could move them to types.h, or items.h? */
#include "statements.h"

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

/* Allocate a shared buffer big enough to hold count elements each of the
   specified size. */
struct shared_buff shared_buff_alloc(int elem_size, int count) {
    struct shared_buff_header *ptr =
        malloc(sizeof(struct shared_buff_header) + elem_size * count);
    ptr->references = 1;
    ptr->start_offset = 0;
    ptr->count = count;
    ptr->buffer_size = elem_size * count;

    struct shared_buff result = {ptr, 0, count};
    return result;
}

void shared_buff_decrement(
    struct shared_buff_header *ptr,
    struct type *elem_type
) {
    if (!ptr) return;

    ptr->references -= 1;
    if (ptr->references <= 0) {
        if (elem_type->connective == TYPE_ARRAY) {
            uint8 *buff_start = (uint8*)&ptr[1];
            struct shared_buff *arr =
                (struct shared_buff*)(buff_start + ptr->start_offset);
            for (int i = 0; i < ptr->count; i++) {
                shared_buff_decrement(arr[i].ptr, elem_type->inner);
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

    frame->locals_start = stack->vars.global_count;
    frame->locals_count = 0;
}

/* Try to decode the ref, but only crash if it is corrupted, not if it is
   REF_NULL. It isn't the interpreter's job to make sure that REF_NULL is used
   correctly. */
uint64 read_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref
) {
    switch (ref.type) {
    case REF_NULL:
        return 0;
    case REF_CONSTANT:
        return ref.x;
    case REF_GLOBAL:
        return vars->data[ref.x].value.val64;
    case REF_LOCAL:
        return vars->data[frame->locals_start + ref.x].value.val64;
    case REF_TEMPORARY:
        return vars->data[frame->locals_start + frame->locals_count + ref.x].value.val64;
    default:
        fprintf(stderr, "Unexpected ref.type value %d?\n", ref.type);
        exit(EXIT_FAILURE);
    }
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
        fprintf(stderr, "Error: ref type not set?\n");
        exit(EXIT_FAILURE);
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

void continue_execution(struct call_stack *stack) {
    while (stack->exec.count > 0) {
        struct execution_frame *frame = buffer_top(stack->exec);
        if (frame->current < 0 || frame->current >= frame->count) {
            buffer_pop(stack->exec);
            continue;
        }

        struct instruction *next = &frame->start[frame->current];

        /* Execute instruction. */
        int64 arg1 = read_ref(frame, &stack->vars, next->arg1);
        int64 arg2 = read_ref(frame, &stack->vars, next->arg2);
        union variable_contents result = {0};
        enum variable_memory_mode result_mode = VARIABLE_DIRECT_VALUE;
        switch (next->op) {
        case OP_NULL:
            break;
        case OP_MOV:
            result.val64 = arg1;
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
            result.shared_buff = shared_buff_alloc(arg1, arg2);
            result_mode = VARIABLE_REFCOUNT;
            break;
        case OP_ARRAY_STORE:
            fprintf(stderr, "Warning: Array store is not yet implemented.\n");
            break;
        default:
            fprintf(stderr, "Error: Tried to execute unknown opcode %d.\n",
                next->op);
            exit(EXIT_FAILURE);
        }

        if (next->arg1.type == REF_TEMPORARY) {
            size_t index = frame->locals_start + frame->locals_count
                + next->arg1.x;
            stack->vars.data[index].mem_mode = VARIABLE_UNBOUND;
        }
        if (next->arg2.type == REF_TEMPORARY) {
            size_t index = frame->locals_start + frame->locals_count
                + next->arg2.x;
            stack->vars.data[index].mem_mode = VARIABLE_UNBOUND;
        }

        write_ref(frame, &stack->vars, next->output, result, result_mode);

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

    call_stack_push_exec_frame(stack, statement_code);

    continue_execution(stack);
}

#endif
