#ifndef MODLANG_BUILTINS_H
#define MODLANG_BUILTINS_H

#include "types.h"
#include "interpreter.h"

union variable_contents add_procedure(
    struct procedure_buffer *procedures,
    struct instruction_buffer instructions
) {
    struct procedure *p = buffer_addn(*procedures, 1);
    p->instructions = instructions;

    union variable_contents result;
    result.val64 = procedures->count - 1;
    return result;
}

void bind_global(
    struct record_table *bindings,
    struct call_stack *call_stack,
    struct record_entry binding,
    union variable_contents val
) {
    buffer_push(*bindings, binding);
    bindings->global_count = bindings->count;

    struct variable_data *var = buffer_addn(call_stack->vars, 1);
    var->value = val;
    call_stack->vars.global_count = bindings->global_count;

}

void bind_procedure(
    struct record_table *bindings,
    struct procedure_buffer *procedures,
    struct call_stack *call_stack,
    struct record_entry proc_binding,
    struct instruction_buffer instructions
) {
    union variable_contents val = add_procedure(procedures, instructions);
    bind_global(bindings, call_stack, proc_binding, val);
}

void add_builtins(
    struct record_table *bindings,
    struct procedure_buffer *procedures,
    struct call_stack *call_stack
) {
    {
        struct type_buffer inputs = {0};
        buffer_push(inputs, type_int64);
        struct type_buffer outputs = {0};

        struct record_entry b = {0};
        b.name = from_cstr("assert");
        b.type = type_proc(inputs, outputs);
        b.is_var = false;

        struct instruction instr = {0};
        instr.op = OP_ASSERT;
        instr.arg1.type = REF_LOCAL;
        instr.arg1.x = 0;

        struct instruction_buffer i = {0};
        buffer_push(i, instr);

        bind_procedure(bindings, procedures, call_stack, b, i);
    }
}

#endif
