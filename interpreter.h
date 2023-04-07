#ifndef MODLANG_INTERPRETER_H
#define MODLANG_INTERPRETER_H

#include "types.h"
/* We really just need the items. We could move them to types.h, or items.h? */
#include "statements.h"

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

uint64 read_ref(
    struct execution_frame *frame,
    struct variable_stack *vars,
    struct ref ref
) {
    switch (ref.type) {
    case REF_NULL:
        fprintf(stderr, "Error: ref type not set?\n");
        exit(EXIT_FAILURE);
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
    uint64 value
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
    vars->data[index].value.val64 = value;
    vars->data[index].mem_mode = VARIABLE_DIRECT_VALUE;
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
        int64 arg2 = 0;
        if (next->op != OP_MOV && next->op <= OP_EMOD) {
            arg2 = read_ref(frame, &stack->vars, next->arg2);
        }
        int64 result = 0;
        switch (next->op) {
        case OP_NULL:
            break;
        case OP_MOV:
            result = arg1;
            break;
        case OP_LOR:
            result = arg1 || arg2;
            break;
        case OP_LAND:
            result = arg1 && arg2;
            break;
        case OP_EQ:
            result = arg1 == arg2;
            break;
        case OP_NEQ:
            result = arg1 != arg2;
            break;
        case OP_LEQ:
            result = arg1 <= arg2;
            break;
        case OP_GEQ:
            result = arg1 >= arg2;
            break;
        case OP_LESS:
            result = arg1 < arg2;
            break;
        case OP_GREATER:
            result = arg1 > arg2;
            break;
        case OP_BOR:
            result = arg1 | arg2;
            break;
        case OP_BAND:
            result = arg1 & arg2;
            break;
        case OP_BXOR:
            result = arg1 ^ arg2;
            break;
        case OP_PLUS:
            result = arg1 + arg2;
            break;
        case OP_MINUS:
            result = arg1 - arg2;
            break;
        case OP_LSHIFT:
            result = arg1 << arg2;
            break;
        case OP_RSHIFT:
            result = arg1 >> arg2;
            break;
        case OP_MUL:
            result = arg1 * arg2;
            break;
        case OP_DIV:
            result = arg1 / arg2;
            break;
        case OP_MOD:
            result = arg1 % arg2;
            break;
        case OP_EDIV:
            if (arg1 >= 0) result = arg1 / arg2;
            else result = (arg1 - arg2 + 1) / arg2;
            break;
        case OP_EMOD:
            if (arg1 >= 0) result = arg1 % arg2;
            else result = arg2 - 1 - (-arg1 - 1) % arg2;
            /* e.g. (-17) mod 10, -17 -> 16 -> 6 -> 3
               if we didn't subtract 1 then we would get (-10) mod 10 = 10 */
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

        write_ref(frame, &stack->vars, next->output, result);

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
