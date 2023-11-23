#ifndef MODLANG_COMPILER_PRIMITIVES_H
#define MODLANG_COMPILER_PRIMITIVES_H

#include "types.h"

struct operator_info {
    enum token_id token;
    enum operation opcode;
    bool word;
    bool floats;
    bool signed_int;
    bool unsigned_int;
};

struct operator_info binary_ops[] = {
    {TOKEN_LOGIC_OR, OP_LOR, true, false},
    {TOKEN_LOGIC_AND, OP_LAND, true, false},
    {TOKEN_EQ, OP_EQ, true, true},
    {TOKEN_NEQ, OP_NEQ, true, true},
    {TOKEN_LEQ, OP_LEQ, false, true, true, true},
    {TOKEN_GEQ, OP_GEQ, false, true, true, true},
    {'<', OP_LESS, false, true, true, true},
    {'>', OP_GREATER, false, true, true, true},
    {'|', OP_BOR, true, false},
    {'&', OP_BAND, true, false},
    {'^', OP_BXOR, true, false},
    {'+', OP_PLUS, true, true},
    {'-', OP_MINUS, true, true},
    {TOKEN_LSHIFT, OP_LSHIFT, true, true},
    {TOKEN_RSHIFT, OP_RSHIFT, false, true, true, true},
    {'*', OP_MUL, true, true},
    {'/', OP_DIV, false, true, true, true},
    {'%', OP_MOD, false, true, true, true},
    {TOKEN_CONCAT, OP_ARRAY_CONCAT},
    /* Postfix delimiters are parsed as both the start of grouping, and as a
       binary application/index operation. These entries represent this latter 
       meaning as application/indexing. */
    {'[', OP_ARRAY_INDEX}
};

/*
struct operator_info unary_ops = {
    {'-', OP_NEG, true, true},
    {'~', OP_BNOT, true, false},
    {'*', OP_OPEN, false, false, false, false},
};
*/

struct intermediate {
    struct ref ref;
    struct type type;
};

struct intermediate_buffer {
    struct intermediate *data;
    size_t count;
    size_t capacity;
    size_t temporaries_count;
};

struct intermediate pop_intermediate(struct intermediate_buffer *intermediates) {
    struct intermediate result = buffer_pop(*intermediates);

    if (result.ref.type == REF_TEMPORARY) {
        intermediates->temporaries_count -= 1;
    }

    return result;
}

struct ref push_intermediate(struct intermediate_buffer *intermediates, struct type ty) {
    struct ref result;
    result.type = REF_TEMPORARY;
    result.x = intermediates->temporaries_count;

    struct intermediate *loc = buffer_addn(*intermediates, 1);
    loc->type = ty;
    loc->ref = result;
    intermediates->temporaries_count += 1;

    return result;
}

void compile_value_token(
    struct record_table *bindings,
    struct intermediate_buffer *intermediates,
    struct token *in
) {
    if (in->id == TOKEN_ALPHANUM) {
        int ind = lookup_name(bindings, in->it);
        if (ind == -1) {
            fprintf(stderr, "Error on line %d, %d: \"", in->row, in->column);
            fputstr(in->it, stderr);
            fprintf(stderr, "\" is not defined in this scope.\n");
            exit(EXIT_FAILURE);
        }

        struct intermediate *loc = buffer_addn(*intermediates, 1);
        loc->type = bindings->data[ind].type;
        if (ind < bindings->global_count) {
            loc->ref.type = REF_GLOBAL;
            loc->ref.x = ind;
        } else {
            loc->ref.type = REF_LOCAL;
            loc->ref.x = ind - bindings->global_count;
        }
    } else if (in->id == TOKEN_NUMERIC) {
        int64 value = integer_from_string(in->it);

        struct intermediate *loc = buffer_addn(*intermediates, 1);
        loc->ref.type = REF_CONSTANT;
        loc->ref.x = value;
        loc->type = type_int64;
    } else {
        fprintf(stderr, "Error: Asked to compile \"");
        fputstr(in->it, stderr);
        fprintf(stderr, "\" as an RPN atom?\n");
        exit(EXIT_FAILURE);
    }
}

void compile_mov(
    struct instruction_buffer *out,
    struct ref to,
    struct ref from,
    struct type *ty
) {
    enum operation_flags flags = 0;
    if (ty->connective == TYPE_INT && ty->word_size == 3) {
        flags = OP_64BIT;
    } else if (ty->connective == TYPE_ARRAY) {
        flags = OP_SHARED_BUFF;
    } else if (ty->connective == TYPE_TUPLE || ty->connective == TYPE_RECORD) {
        flags = OP_64BIT;
    } else {
        fprintf(stderr, "Error: Move instructions are only "
            "implemented for arrays and 64 bit integers.\n");
        exit(EXIT_FAILURE);
    }

    struct instruction *instr = buffer_addn(*out, 1);
    instr->op = OP_MOV;
    instr->flags = flags;
    instr->output = to;
    instr->arg1 = from;
    instr->arg2.type = REF_NULL;
}

/* When we write a struct to a location, we then need to increment some of the
   reference counts in that struct. This function will statically generate the
   instructions required to do that. */
void compile_struct_increment(
    struct instruction_buffer *out,
    struct ref val,
    struct type *element_type
) {
    fprintf(stderr, "Warning: copying complex structs is not yet "
        "implemented.\n");
}

struct type compile_store(
    struct instruction_buffer *out,
    struct ref to_ptr,
    int64 offset,
    struct intermediate_buffer *intermediates
) {
    /* Don't pop yet, in case we need to make some more temporaries first. */
    struct intermediate val = *buffer_top(*intermediates);

    enum operation_flags flags = 0;
    if (val.type.connective == TYPE_INT && val.type.word_size == 3) {
        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_POINTER_STORE;
        instr->flags = OP_64BIT;
        instr->output = to_ptr;
        instr->arg1.type = REF_CONSTANT;
        instr->arg1.x = offset;
        instr->arg2 = val.ref;
    } else if (val.type.connective == TYPE_ARRAY) {
        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_POINTER_STORE;
        instr->flags = OP_SHARED_BUFF;
        instr->output = to_ptr;
        instr->arg1.type = REF_CONSTANT;
        instr->arg1.x = offset;
        instr->arg2 = val.ref;
    } else if (val.type.connective == TYPE_TUPLE || val.type.connective == TYPE_RECORD) {
        /* First compile the reference count increments, if any. */
        if (val.ref.type != REF_TEMPORARY) {
            /* If val is a temporary, then we can discard the old memory. If it
               is not a temporary, then we can use the pointer multiple times
               to increment all the arrays inside it, without the pointer
               getting discarded. */
            compile_struct_increment(out, val.ref, &val.type);
        }

        /* Now compile the actual copy operation. */
        struct instruction *instrs = buffer_addn(*out, 2);

        /* TODO: If offset is 0 then we should write directly to the pointer,
           but without discarding it, even though it is temporary... Requires
           actual flags for dealing with temporaries though, instead of what we
           are doing now. */
        /* We immediately use our own temporary, so we don't need to add
           anything to the intermediates buffer. */
        struct ref offset_ptr = {REF_TEMPORARY};
        offset_ptr.x = intermediates->temporaries_count;
        instrs[0].op = OP_POINTER_OFFSET;
        instrs[0].output = offset_ptr;
        instrs[0].arg1 = to_ptr;
        instrs[0].arg2.type = REF_CONSTANT;
        instrs[0].arg2.x = offset;

        instrs[1].op = OP_POINTER_COPY;
        instrs[1].output = offset_ptr;
        instrs[1].arg1 = val.ref;
        instrs[1].arg2.type = REF_CONSTANT;
        instrs[1].arg2.x = val.type.total_size;
    } else {
        fprintf(stderr, "Error: Store instructions are only "
            "implemented for arrays and 64 bit integers.\n");
        exit(EXIT_FAILURE);
    }

    /* Pop AFTER storing, in case we needed to contrive a temporary pointer
       value. */
    pop_intermediate(intermediates);
    return val.type;
}

/* Push the top intermediate value onto the stack, if it isn't already on the
   stack. */
void compile_push(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates
) {
    if (intermediates->count == 0) {
        fprintf(stderr, "Error: Tried to push an intermediate to the stack, "
            "when there were no intermediates?\n");
        exit(EXIT_FAILURE);
    }
    struct intermediate *val = buffer_top(*intermediates);
    if (val->ref.type != REF_TEMPORARY) {
        struct ref to = {REF_TEMPORARY, intermediates->temporaries_count};
        compile_mov(out, to, val->ref, &val->type);
        val->ref = to;
        intermediates->temporaries_count += 1;
    }
}

void compile_operation(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct intermediate_buffer *intermediates,
    struct token operation
) {
    struct operator_info *op = NULL;
    for (int i = 0; i < ARRAY_LENGTH(binary_ops); i++) {
        if (binary_ops[i].token == operation.id) {
            op = &binary_ops[i];
            break;
        }
    }
    if (!op) {
        if (IS_PRINTABLE(operation.id)) {
            fprintf(stderr, "Error: Operator '%c' is not yet "
                "implemented.\n", operation.id);
        } else {
            fprintf(stderr, "Error: Operator id %d is not implemented.\n",
                operation.id);
        }
        exit(EXIT_FAILURE);
    }

    struct intermediate val2 = pop_intermediate(intermediates);
    struct intermediate val1 = pop_intermediate(intermediates);

    struct instruction result;
    result.op = op->opcode;
    result.arg1 = val1.ref;
    result.arg2 = val2.ref;

    struct type result_type = type_int64;

    if (op->opcode == OP_ARRAY_INDEX) {
        /* Casing all the scalar connectives sounds annoying. Maybe I should
           make the connective "scalar", or work out some bit mask trick to
           test them all in one go. */
        if (val1.type.connective != TYPE_ARRAY) {
            fprintf(stderr, "Error: Left side of array index must be an "
                "array.\n");
            exit(EXIT_FAILURE);
        }
        if (val2.type.connective != TYPE_INT) {
            fprintf(stderr, "Error: Array index must be an integer.\n");
            exit(EXIT_FAILURE);
        }
        if (val2.type.word_size != 3) {
            fprintf(stderr, "Error: Currently only 64 bit integer types are "
                    "implemented.\n");
            exit(EXIT_FAILURE);
        }
        struct type *inner = val1.type.inner;
        if (inner->connective == TYPE_ARRAY) {
            result.flags = OP_SHARED_BUFF;
        } else if (inner->connective == TYPE_INT) {
            result.flags = OP_64BIT;
        } else {
            fprintf(stderr, "Error: Currently only arrays of integers or "
                "other arrays can be indexed.\n");
            exit(EXIT_FAILURE);
        }

        result_type = *val1.type.inner;
    } else if (op->opcode == OP_ARRAY_CONCAT) {
        if (val1.type.connective != TYPE_ARRAY || val2.type.connective != TYPE_ARRAY) {
            fprintf(stderr, "Error: Arguments to ++ operator must be "
                "arrays.\n");
            exit(EXIT_FAILURE);
        }

        if (!type_eq(val1.type.inner, val2.type.inner)) {
            fprintf(stderr, "Error: Tried to apply ++ operator to arrays with "
                "different types.\n", operation.row, operation.column);
            exit(EXIT_FAILURE);
        }

        result_type = val1.type;
    } else {
        /* Casing all the scalar connectives sounds annoying. Maybe I should
           make the connective "scalar", or work out some bit mask trick to
           test them all in one go. */
        if (val1.type.connective != TYPE_INT || val2.type.connective != TYPE_INT) {
            fprintf(stderr, "Error: Argument to operator %c must be an integer.\n",
                    operation.id);
            exit(EXIT_FAILURE);
        }
        if (val1.type.word_size != 3 || val2.type.word_size != 3) {
            fprintf(stderr, "Error: Currently only 64 bit integer types are "
                    "implemented.\n");
            exit(EXIT_FAILURE);
        }
        result.flags = OP_64BIT;
    }

    /* We should be managing these intermediate types, but they get shared by
       emplace logic and thus by array_alloc instructions, so really they need
       to be shared, or deep cloned each time. Shared probably makes sense, but
       then what is the lifetime for the code that we generate?? Indefinite? */
    /* if (val1.ref.type == REF_TEMPORARY) destroy_type(&val1.type); */
    /* if (val2.ref.type == REF_TEMPORARY) destroy_type(&val2.type); */

    result.output = push_intermediate(intermediates, result_type);

    buffer_push(*out, result);

}

void compile_proc_call(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates,
    int arg_count
) {
    struct instruction instr = {0};
    instr.op = OP_CALL;
    instr.flags = 0;
    instr.output.type = REF_NULL;

    size_t proc_index = intermediates->count - arg_count - 1;
    struct intermediate proc_val = intermediates->data[proc_index];
    /* This is supposed to be a valid procedure, we will see if it actually has
       the right type, though. */
    instr.arg1 = proc_val.ref;

    if (proc_val.type.connective != TYPE_PROCEDURE) {
        /* TODO: Get a row/column here somehow */
        fprintf(stderr, "Error: Tried to call something that "
            "wasn't a function or procedure.\n");
        exit(EXIT_FAILURE);
    }
    struct type_buffer inputs = proc_val.type.proc.inputs;
    struct type_buffer outputs = proc_val.type.proc.outputs;

    if (inputs.count != arg_count) {
        /* TODO: Get a row/column here somehow */
        fprintf(stderr, "Error: Procedure expected %d arguments, but %d were "
            "given.\n", (int)inputs.count, (int)arg_count);
        exit(EXIT_FAILURE);
    }

    struct intermediate *actual_types =
        &intermediates->data[intermediates->count - arg_count];

    for (int i = 0; i < inputs.count; i++) {
        if (!type_eq(&inputs.data[i], &actual_types[i].type)) {
            /* TODO: Get a row/column here somehow */
            fprintf(stderr, "Error: Argument %d of function call had the "
                "wrong type.\n", i + 1);
            exit(EXIT_FAILURE);
        }
    }

    instr.arg2.type = REF_CONSTANT;
    instr.arg2.x = arg_count;

    buffer_push(*out, instr);

    /* discard args */
    intermediates->count -= arg_count;
    intermediates->temporaries_count -= arg_count;
    /* discard proc */
    pop_intermediate(intermediates);

    /* add result */
    buffer_maybe_grow(*intermediates, outputs.count);
    for (int i = 0; i < outputs.count; i++) {
        push_intermediate(intermediates, outputs.data[i]);
    }
}

void compile_return(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates
) {
    size_t val_count = intermediates->count;
    if (val_count > 1) {
        /* To implement multivalue return statements we would need to pass a
           boolean into expression compilation, to allocate all things
           immediately to the stack. This is similar to what we do for function
           arguments, but at the top level of the expression now. */
        fprintf(stderr, "Error: Multivalue return statements are not yet "
            "implemented.\n");
        exit(EXIT_FAILURE);
    }
    if (val_count == 1) {
        compile_push(out, intermediates);
    }

    struct instruction *instr = buffer_addn(*out, 1);
    instr->op = OP_RET;
    instr->flags = 0;
    instr->output.type = REF_NULL;
    instr->arg1.type = REF_CONSTANT;
    instr->arg1.x = val_count;
    instr->arg2.type = REF_NULL;
}

void type_check_return(
    struct type_buffer *expected,
    struct intermediate_buffer *actual,
    str proc_name
) {
    if (expected->count != actual->count) {
        fprintf(stderr, "Error: Function \"");
        fputstr(proc_name, stderr);
        fprintf(stderr, "\" should return %d values, but %d were "
            "given.\n", (int)expected->count,
            (int)actual->count);
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < actual->count; i++) {
        if (!type_eq(&actual->data[i].type, &expected->data[i])) {
            fprintf(stderr, "Error: Return value %d of function \"",
                i + 1);
            fputstr(proc_name, stderr);
            fprintf(stderr, "\" had the wrong type.\n");
            exit(EXIT_FAILURE);
        }
    }
}

#endif
