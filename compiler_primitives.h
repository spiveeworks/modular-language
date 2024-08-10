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
    size_t ref_offset;
    bool is_pointer;
    bool owns_stack_memory;
    bool stack_offset_known;
    size_t alloc_size;
    size_t temp_stack_offset;
};

struct intermediate_buffer {
    struct intermediate *data;
    size_t count;
    size_t capacity;
    /* We could store the first temporary index and the number, but this is all
       we actually need for allocating temporaries. */
    size_t next_local_index;
};

struct intermediate_buffer intermediates_start(struct record_table *bindings) {
    size_t local_count = bindings->count - bindings->global_count + bindings->out_ptr_count;
    return (struct intermediate_buffer){.next_local_index = local_count};
}

struct intermediate pop_intermediate(struct intermediate_buffer *intermediates) {
    struct intermediate result = buffer_pop(*intermediates);

    if (result.ref.type == REF_TEMPORARY) {
        intermediates->next_local_index -= 1;
    }

    return result;
}

struct ref push_intermediate(struct intermediate_buffer *intermediates, struct type ty) {
    struct ref result;
    result.type = REF_TEMPORARY;
    result.x = intermediates->next_local_index;

    struct intermediate *loc = buffer_addn(*intermediates, 1);
    *loc = (struct intermediate){0};
    loc->type = ty;
    loc->alloc_size = ty.total_size;
    loc->ref = result;
    loc->is_pointer = ty.connective == TYPE_RECORD || ty.connective == TYPE_TUPLE;
    intermediates->next_local_index += 1;

    return result;
}

struct ref variable_index_ref(struct record_table *bindings, int ind) {
    struct ref result;

    if (ind < bindings->global_count) {
        result.type = REF_GLOBAL;
        result.x = ind;
    } else if (ind < bindings->global_count + bindings->arg_count) {
        result.type = REF_LOCAL;
        result.x = ind - bindings->global_count;
    } else {
        result.type = REF_LOCAL;
        result.x = ind - bindings->global_count + bindings->out_ptr_count;
    }

    return result;
}

void scope_error(struct token *tk) {
    fprintf(stderr, "Error on line %d, %d: \"", tk->row, tk->column);
    fputstr(tk->it, stderr);
    fprintf(stderr, "\" is not defined in this scope.\n");
    exit(EXIT_FAILURE);
}

struct record_entry *convert_name(
    struct record_table *bindings,
    struct token *in,
    struct type *type_out,
    struct ref *ref_out
) {
    int ind = lookup_name(bindings, in->it);
    if (ind == -1) scope_error(in);

    struct record_entry *binding = &bindings->data[ind];

    if (type_out) *type_out = binding->type;
    if (ref_out) *ref_out = variable_index_ref(bindings, ind);

    return binding;
}

void compile_value_token(
    struct record_table *bindings,
    struct intermediate_buffer *intermediates,
    struct token *in
) {
    if (in->id == TOKEN_ALPHANUM) {
        struct intermediate *loc = buffer_addn(*intermediates, 1);
        *loc = (struct intermediate){0};

        convert_name(bindings, in, &loc->type, &loc->ref);

        if (loc->type.connective == TYPE_RECORD || loc->type.connective == TYPE_TUPLE) {
            loc->is_pointer = true;
        }
    } else if (in->id == TOKEN_NUMERIC) {
        int64 value = integer_from_string(in->it);

        struct intermediate *loc = buffer_addn(*intermediates, 1);
        *loc = (struct intermediate){0};
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

void compile_mov_ref(
    struct instruction_buffer *out,
    struct ref to,
    struct ref from,
    struct type *ty,
    bool force_pointer
) {
    enum operation_flags flags = 0;
    if (force_pointer || ty->connective == TYPE_TUPLE || ty->connective == TYPE_RECORD) {
        flags = OP_64BIT;
    } else if (ty->connective == TYPE_INT && ty->word_size == 3) {
        flags = OP_64BIT;
    } else if (ty->connective == TYPE_ARRAY) {
        flags = OP_SHARED_BUFF;
    } else if (ty->connective == TYPE_PROCEDURE) {
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

void compile_mov(
    struct instruction_buffer *out,
    struct ref to,
    struct intermediate *from
) {
    compile_mov_ref(out, to, from->ref, &from->type, from->is_pointer);
}

/* When we write a struct to a location, we then need to increment some of the
   reference counts in that struct. This function will statically generate the
   instructions required to do that. */
void compile_pointer_refcounts(
    struct instruction_buffer *out,
    struct ref val,
    size_t offset,
    struct type *element_type,
    bool decrement /* The same logic can also be used for decrements. */
) {
    if (element_type->connective == TYPE_ARRAY) {
        struct instruction *instr = buffer_addn(*out, 1);
        if (decrement) instr->op = OP_POINTER_DECREMENT_REFCOUNT;
        else instr->op = OP_POINTER_INCREMENT_REFCOUNT;
        instr->output.type = REF_NULL;
        instr->arg1 = val;
        instr->arg2.type = REF_CONSTANT;
        instr->arg2.x = offset;
    } else if (element_type->connective == TYPE_TUPLE) {
        for (int i = 0; i < element_type->elements.count; i++) {
            struct type *it = &element_type->elements.data[i];
            compile_pointer_refcounts(out, val, offset, it, decrement);
            offset += it->total_size;
        }
    } else if (element_type->connective == TYPE_RECORD) {
        for (int i = 0; i < element_type->fields.count; i++) {
            struct type *it = &element_type->fields.data[i].type;
            compile_pointer_refcounts(out, val, offset, it, decrement);
            offset += it->total_size;
        }
    } else if (element_type->connective != TYPE_INT) {
        fprintf(stderr, "Warning: copying type connective %d is not yet "
            "implemented.\n", element_type->connective);
    }
}

void compile_copy(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates,
    struct ref to_ptr,
    struct intermediate *from_ptr,
    bool allocate_output
) {
    struct ref from_ptr_offset = from_ptr->ref;
    bool pushed_new = false;
    if (from_ptr->ref_offset != 0) {
        /* We need to offset the pointer first. Pick a register to store the
           offset version of the pointer in. */
        if (from_ptr->ref.type != REF_TEMPORARY || from_ptr->owns_stack_memory) {
            /* from_ptr.ref is a variable or a struct literal, so allocate a
               new temporary to store the pointer in. */
            from_ptr_offset = push_intermediate(intermediates, from_ptr->type);
            pushed_new = true;
        } /* else we don't need from_ptr.ref, so just use it. */

        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_POINTER_OFFSET;
        instr->output = from_ptr_offset;
        instr->arg1 = from_ptr->ref;
        instr->arg2.type = REF_CONSTANT;
        instr->arg2.x = from_ptr->ref_offset;
    }

    struct instruction *instr = buffer_addn(*out, 1);
    if (allocate_output) instr->op = OP_POINTER_DUP;
    else instr->op = OP_POINTER_COPY;
    instr->output = to_ptr;
    instr->arg1 = from_ptr_offset;
    instr->arg2.type = REF_CONSTANT;
    instr->arg2.x = from_ptr->type.total_size;

    if (from_ptr->owns_stack_memory) {
        /* Use the contents of the struct as is, and just free the struct. */
        instr = buffer_addn(*out, 1);
        instr->op = OP_STACK_FREE;
        instr->output.type = REF_NULL;
        instr->arg1 = from_ptr->ref;
        instr->arg2.type = REF_NULL;
    } else {
        /* It is not a temporary, so sweep through it to increment any
           reference counted pointers that were just copied. */
        compile_pointer_refcounts(out, from_ptr->ref, 0, &from_ptr->type, false);
    }

    if (pushed_new) pop_intermediate(intermediates);
}

/* Move a struct backwards in the stack, in cases where a temporary was
   constructed and immediately indexed. */
void realloc_temp_struct(
    struct instruction_buffer *out,
    struct intermediate_buffer *values,
    struct intermediate *val
) {
    if (val->ref_offset > 0) {
        /* Move the data to the left. */
        struct ref offset_ptr = push_intermediate(values, val->type);
        struct instruction *instrs = buffer_addn(*out, 2);
        instrs[0].op = OP_POINTER_OFFSET;
        instrs[0].flags = 0;
        instrs[0].output = offset_ptr;
        instrs[0].arg1 = val->ref;
        instrs[0].arg2.type = REF_CONSTANT;
        instrs[0].arg2.x = val->ref_offset;

        if (val->type.total_size <= val->ref_offset) {
            instrs[1].op = OP_POINTER_COPY;
        } else {
            instrs[1].op = OP_POINTER_COPY_OVERLAPPING;
        }
        instrs[1].flags = 0;
        instrs[1].output = val->ref;
        instrs[1].arg1 = offset_ptr;
        instrs[1].arg2.type = REF_CONSTANT;
        instrs[1].arg2.x = val->type.total_size;

        pop_intermediate(values);
    }
    /* Free the unused part. */
    struct ref offset_ptr = push_intermediate(values, type_empty_tuple);
    struct instruction *instrs = buffer_addn(*out, 2);
    instrs[0].op = OP_POINTER_OFFSET;
    instrs[0].flags = 0;
    instrs[0].output = offset_ptr;
    instrs[0].arg1 = val->ref;
    instrs[0].arg2.type = REF_CONSTANT;
    instrs[0].arg2.x = val->type.total_size;

    instrs[1].op = OP_STACK_FREE;
    instrs[1].flags = 0;
    instrs[1].output.type = REF_NULL;
    instrs[1].arg1 = offset_ptr;
    instrs[1].arg2.type = REF_NULL;

    pop_intermediate(values);
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
    if (val.is_pointer) {
        if (val.type.connective != TYPE_TUPLE && val.type.connective != TYPE_RECORD) {
            fprintf(stderr, "Error: Tried to store a pointer that was pointing to a scalar?\n");
            exit(EXIT_FAILURE);
        }

        struct ref offset_ptr = push_intermediate(intermediates, val.type);

        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_POINTER_OFFSET;
        instr->output = offset_ptr;
        instr->arg1 = to_ptr;
        instr->arg2.type = REF_CONSTANT;
        instr->arg2.x = offset;

        compile_copy(out, intermediates, offset_ptr, &val, false);

        pop_intermediate(intermediates);
    } else if (val.type.connective == TYPE_INT && val.type.word_size == 3) {
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
        struct ref to = {REF_TEMPORARY, intermediates->next_local_index};
        compile_mov(out, to, val);
        val->ref = to;
        intermediates->next_local_index += 1;
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
        } else if (inner->connective == TYPE_PROCEDURE) {
            /* TODO: Make procedures have enclosed state, and handle that
               appropriately. */
            result.flags = OP_64BIT;
        } else {
            /* Not a scalar, so give a pointer instead. TODO: Break this up
               into some kind of MUL ADD with more steps, to stop relying on
               reflection to index arrays?? */
            result.op = OP_ARRAY_OFFSET;
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

void compile_struct_member(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct intermediate_buffer *intermediates,
    struct token member_tk
) {
    struct intermediate *it = buffer_top(*intermediates);
    struct type *member_ty = NULL;
    size_t offset = it->ref_offset;
    int64 member_index;
    if (it->type.connective == TYPE_TUPLE) {
        if (member_tk.id != TOKEN_NUMERIC) {
            fprintf(stderr, "Error at line %d, %d: Tried to access the "
                "field \"", member_tk.row, member_tk.column);
            fputstr(member_tk.it, stderr);
            fprintf(stderr, "\" in a tuple type.\n");
            exit(EXIT_FAILURE);
        }

        member_index = integer_from_string(member_tk.it);
        if (member_index >= it->type.elements.count) {
            fprintf(stderr, "Error at line %d, %d: Tried to access element "
                "%lld of a tuple with only %llu elements.\n",
                member_tk.row, member_tk.column,
                member_index, it->type.elements.count);
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < member_index; i++) {
            offset += it->type.elements.data[i].total_size;
        }
        member_ty = &it->type.elements.data[member_index];
    } else if (it->type.connective == TYPE_RECORD) {
        member_index = lookup_name_fields(&it->type.fields, member_tk.it);
        if (member_index == -1) {
            fprintf(stderr, "Error at line %d, %d: Tried to access field \"", member_tk.row, member_tk.column);
            fputstr(member_tk.it, stderr);
            fprintf(stderr, "\" of a record type that does not have that "
                "field.\n");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < member_index; i++) {
            offset += it->type.fields.data[i].type.total_size;
        }
        member_ty = &it->type.fields.data[member_index].type;
    } else {
        fprintf(stderr, "Error at line %d, %d: Tried to access a member of "
            "something that wasn't a tuple or record type.\n",
            member_tk.row, member_tk.column);
        exit(EXIT_FAILURE);
    }

    if (member_ty->connective == TYPE_INT || member_ty->connective == TYPE_ARRAY) {
        enum operation_flags flags = 0;
        if (member_ty->connective == TYPE_ARRAY) flags = OP_SHARED_BUFF;
        else flags = OP_64BIT;

        if (it->owns_stack_memory) {
            /* Reading a scalar from a struct literal, load the value, and then
               destroy the struct. */
            if (it->ref.type != REF_TEMPORARY) {
                fprintf(stderr, "Internal error: Got an intermediate that "
                    "owns stack memory, but isn't a temporary?\n");
                exit(EXIT_FAILURE);
            }

            /* `output` should be the same ref as `it.ref`, but we want to free
               `it.ref`. Load to an additional temporary first. */
            struct ref tmp = push_intermediate(intermediates, *member_ty);

            /* Load it to tmp */
            struct instruction *instrs = buffer_addn(*out, 1);
            instrs[0].op = OP_POINTER_LOAD;
            instrs[0].flags = flags;
            instrs[0].output = tmp;
            instrs[0].arg1 = it->ref;
            instrs[0].arg2.type = REF_CONSTANT;
            instrs[0].arg2.x = offset;

            /* Destroy and free it */
            compile_pointer_refcounts(
                out,
                it->ref,
                it->ref_offset,
                &it->type,
                true
            );
            instrs = buffer_addn(*out, 2);
            instrs[0].op = OP_STACK_FREE;
            instrs[0].flags = 0;
            instrs[0].output.type = REF_NULL;
            instrs[0].arg1 = it->ref;
            instrs[0].arg2.type = REF_NULL;

            /* Pop both the temporary and the pointer, and push the actual
               output. */
            pop_intermediate(intermediates);
            pop_intermediate(intermediates);
            struct ref output = push_intermediate(intermediates, *member_ty);

            /* Move tmp to output (which was probably it.ref all along) */
            instrs[1].op = OP_MOV;
            instrs[1].flags = flags;
            instrs[1].output = output;
            instrs[1].arg1 = tmp;
            instrs[1].arg2.type = REF_NULL;
        } else {
            /* Nothing to free, so just read it out. */
            struct ref it_ref = it->ref;
            pop_intermediate(intermediates);
            struct ref output = push_intermediate(intermediates, *member_ty);

            struct instruction *instr = buffer_addn(*out, 1);
            instr->op = OP_POINTER_LOAD;
            instr->flags = flags;
            instr->output = output;
            instr->arg1 = it_ref;
            instr->arg2.type = REF_CONSTANT;
            instr->arg2.x = offset;
        }
    } else {
        if (it->owns_stack_memory) {
            if (it->type.connective == TYPE_TUPLE) {
                /* We are indexing into a struct literal, deinitialize
                   everything except this element. */
                size_t dealloc_offset = it->ref_offset;
                for (int i = 0; i < it->type.elements.count; i++) {
                    struct type *element_type = &it->type.elements.data[i];
                    if (i != member_index) {
                        compile_pointer_refcounts(
                            out,
                            it->ref,
                            dealloc_offset, /* offset from it->ref */
                            element_type,
                            true /* lower refcounts, rather than increase */
                        );
                    }
                    dealloc_offset += element_type->total_size;
                }
            } else if (it->type.connective == TYPE_RECORD) {
                /* We are indexing into a struct literal, deinitialize
                   everything except this element. */
                size_t dealloc_offset = it->ref_offset;
                for (int i = 0; i < it->type.fields.count; i++) {
                    if (i == member_index) continue;

                    struct type *element_type = &it->type.fields.data[i].type;
                    compile_pointer_refcounts(
                        out,
                        it->ref,
                        dealloc_offset, /* offset from it->ref */
                        element_type,
                        true /* lower refcounts, rather than increase */
                    );
                    dealloc_offset += element_type->total_size;
                }
            }
        }

        /* We don't want to load anything yet, and the intermediate buffer can
           take offsets, so just update the offset. */
        it->ref_offset = offset;
        it->type = *member_ty;
    }
}

/* TODO: Move the definitions here. */
void compile_variable_decrements(
    struct instruction_buffer *out,
    struct ref it,
    struct type *type,
    size_t ref_offset,
    bool destroy_structs,
    bool free_structs
);

struct proc_call_info {
    size_t output_bytes;
    int arg_count;
    bool has_input_memory;
    bool keep_output_memory;
    struct ref temp_memory;
};

void compile_proc_call(
    struct instruction_buffer *out,
    int local_count,
    struct intermediate_buffer *intermediates,
    struct proc_call_info *call
) {
    size_t proc_index = intermediates->count - call->arg_count - 1;
    struct intermediate proc_val = intermediates->data[proc_index];

    if (proc_val.type.connective != TYPE_PROCEDURE) {
        /* TODO: Get a row/column here somehow */
        fprintf(stderr, "Error: Tried to call something that "
            "wasn't a function or procedure.\n");
        exit(EXIT_FAILURE);
    }
    struct type_buffer inputs = proc_val.type.proc.inputs;
    struct type_buffer outputs = proc_val.type.proc.outputs;

    if (inputs.count != call->arg_count) {
        /* TODO: Get a row/column here somehow */
        fprintf(stderr, "Error: Procedure expected %d arguments, but %d were "
            "given.\n", (int)inputs.count, (int)call->arg_count);
        exit(EXIT_FAILURE);
    }

    struct intermediate *actual_types =
        &intermediates->data[intermediates->count - call->arg_count];

    for (int i = 0; i < inputs.count; i++) {
        if (!type_eq(&inputs.data[i], &actual_types[i].type)) {
            /* TODO: Get a row/column here somehow */
            fprintf(stderr, "Error: Argument %d of function call had the "
                "wrong type.\n", i + 1);
            exit(EXIT_FAILURE);
        }
    }

    if (call->keep_output_memory) {
        size_t curr_offset = 0;
        for (int i = 0; i < outputs.count; i++) {
            struct type *out_type = &outputs.data[i];
            if (out_type->connective != TYPE_TUPLE && out_type->connective != TYPE_RECORD) {
                continue;
            }

            struct instruction *instr = buffer_addn(*out, 1);
            instr->op = OP_POINTER_OFFSET;
            instr->flags = 0;
            instr->output.type = REF_TEMPORARY;
            instr->output.x = intermediates->next_local_index + i;
            instr->arg1 = call->temp_memory;
            instr->arg2.type = REF_CONSTANT;
            instr->arg2.x = curr_offset;

            curr_offset += out_type->total_size;
        }
    }

    struct instruction instr = {0};
    instr.op = OP_CALL;
    instr.flags = 0;
    instr.output.type = REF_NULL;
    instr.arg1 = proc_val.ref;
    instr.arg2.type = REF_CONSTANT;
    instr.arg2.x = intermediates->next_local_index - call->arg_count;
    buffer_push(*out, instr);

    /* discard args */
    if (call->has_input_memory) {
        /* We can destroy the input structs without worrying about the outputs,
           since compile_variable_decrements works in-place using
           OP_POINTER_DECREMENT_REFCOUNT */
        size_t curr_offset = call->output_bytes;
        for (int i = 0; i < inputs.count; i++) {
            struct intermediate *it = &actual_types[i];
            if (it->owns_stack_memory) {
                /* Functions don't destroy/free structs that are passed to them, so
                   we have to destroy them ourselves. */
                /* We are destroying these terms left to right, so we don't want to
                   free them, instead we free them all in one go at the end. */
                compile_variable_decrements(out, call->temp_memory, &it->type, curr_offset + it->ref_offset, true, false);
                curr_offset += it->type.total_size;
            }
        }
    }
    intermediates->count -= call->arg_count;
    intermediates->next_local_index -= call->arg_count;

    /* discard proc */
    pop_intermediate(intermediates);

    /* remove temp pointer */
    if (call->keep_output_memory) {
        /* We have structs to return, so we want to use the temp_memory pointer
           as the output. */
        if (outputs.count > 1) {
            fprintf(stderr, "Error: Structs in multivalue function results are not yet implemented.\n");
        }
        if (outputs.count == 0) {
            fprintf(stderr, "Error: Function was called with keep_output_memory had no outputs?\n");
            exit(EXIT_FAILURE);
        }

        if (call->has_input_memory) {
            /* We need to free some temp inputs, but we also need to keep the
               temp pointer since it points to the output. */
            struct ref inputs_ptr = {REF_TEMPORARY, intermediates->next_local_index};
            struct instruction *instrs = buffer_addn(*out, 2);
            instrs[0].op = OP_POINTER_OFFSET;
            instrs[0].flags = OP_64BIT;
            instrs[0].output = inputs_ptr;
            instrs[0].arg1 = call->temp_memory;
            instrs[0].arg2.type = REF_CONSTANT;
            instrs[0].arg2.x = call->output_bytes;

            instrs[1].op = OP_STACK_FREE;
            instrs[1].flags = 0;
            instrs[1].output.type = REF_NULL;
            instrs[1].arg1 = inputs_ptr;
            instrs[1].arg2.type = REF_NULL;
        }

        /* If there is only one output, we already have the pointer to it on
           the stack, so we don't need to do any rearrangements. Just make this
           pointer visible by giving it a type. */
        intermediates->next_local_index -= 1;
        push_intermediate(intermediates, outputs.data[0]);
        struct intermediate *val = buffer_top(*intermediates);
        val->owns_stack_memory = true;
    } else if (call->has_input_memory) {
        /* We want to discard the temp memory altogether, so free it, and move
           all the outputs back one index. */
        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_STACK_FREE;
        instr->flags = 0;
        instr->output.type = REF_NULL;
        instr->arg1 = call->temp_memory;
        instr->arg2.type = REF_NULL;

        intermediates->next_local_index -= 1;

        for (int i = 0; i < outputs.count; i++) {
            struct ref to = {REF_TEMPORARY, intermediates->next_local_index};
            struct ref from = {REF_TEMPORARY, intermediates->next_local_index + 1};
            compile_mov_ref(out, to, from, &outputs.data[i], false);
            push_intermediate(intermediates, outputs.data[i]);
        }
    } else {
        /* No temp memory to worry about, just add the outputs to the
           intermediates buffer. */
        buffer_maybe_grow(*intermediates, outputs.count);
        for (int i = 0; i < outputs.count; i++) {
            /* TODO: What if the outputs are tuples/records? */
            push_intermediate(intermediates, outputs.data[i]);
        }
    }
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

void compile_variable_decrements(
    struct instruction_buffer *out,
    struct ref it,
    struct type *type,
    size_t ref_offset,
    bool destroy_structs,
    /* TODO: Make this the caller's responsibility? */
    bool free_structs
) {
    if (type->connective == TYPE_ARRAY) {
        struct instruction *instr = buffer_addn(*out, 1);
        instr->op = OP_DECREMENT_REFCOUNT;
        instr->flags = 0;
        instr->output.type = REF_NULL;
        instr->arg1 = it;
        instr->arg2.type = REF_NULL;
    } else if (type->connective == TYPE_TUPLE || type->connective == TYPE_RECORD) {
        if (destroy_structs) {
            compile_pointer_refcounts(out, it, ref_offset, type, true);
        }
        if (free_structs) {
            struct instruction *instr = buffer_addn(*out, 1);
            instr->op = OP_STACK_FREE;
            instr->flags = 0;
            instr->output.type = REF_NULL;
            instr->arg1 = it;
            instr->arg2.type = REF_NULL;
        }
    } else if (type->connective != TYPE_INT) {
        fprintf(stderr, "Warning: Unknown type will be put on the stack, it "
            "may leak memory.\n");
    }
}

void compile_local_decrements(
    struct instruction_buffer *out,
    struct record_table *bindings
) {
    int64 local_count = bindings->count - bindings->global_count;
    for (int64 i = local_count - 1; i >= 0; i--) {
        struct record_entry *it = &bindings->data[bindings->global_count + i];
        struct ref ref = {REF_LOCAL, i + bindings->out_ptr_count};
        bool is_arg = i < bindings->arg_count;
        /* TODO: Combine all of the stack free operations into one? */
        compile_variable_decrements(out, ref, &it->type, 0, !is_arg, !is_arg);
    }
}

void compile_return(
    struct instruction_buffer *out,
    struct record_table *bindings,
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
        struct type *result_type = &intermediates->data[0].type;
        if (result_type->connective == TYPE_TUPLE || result_type->connective == TYPE_RECORD) {
            if (bindings->out_ptr_count != 1) {
                /* TODO: Better error message for when this is actually
                   reachable? */
                fprintf(stderr, "Error: Expected %llu struct results, but got 1.\n", bindings->out_ptr_count);
                exit(EXIT_FAILURE);
            }
            struct ref out_ptr = {REF_LOCAL, bindings->arg_count};
            compile_copy(
                out,
                intermediates,
                out_ptr,
                &intermediates->data[0],
                false
            );

            /* This function doesn't actually return any scalars, so we want to
               RET 0, not RET 1. */
            val_count -= 1;
        } else {
            if (intermediates->data[0].is_pointer) {
                fprintf(stderr, "Error: tried to return a pointer that was pointing to a scalar?\n");
                exit(EXIT_FAILURE);
            }
            if (bindings->out_ptr_count != 0) {
                /* TODO: Better error message for when this is actually
                   reachable? */
                fprintf(stderr, "Error: Expected %llu struct results, but got none.\n", bindings->out_ptr_count);
                exit(EXIT_FAILURE);
            }
            compile_push(out, intermediates);
        }
    }

    compile_local_decrements(out, bindings);

    struct instruction *instr = buffer_addn(*out, 1);
    instr->op = OP_RET;
    instr->flags = 0;
    instr->output.type = REF_NULL;
    instr->arg1.type = REF_CONSTANT;
    instr->arg1.x = bindings->count - bindings->global_count;
    instr->arg2.type = REF_CONSTANT;
    instr->arg2.x = val_count;
}

void compile_multivalue_decrements(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates
) {
    while (intermediates->count > 0) {
        struct intermediate it = buffer_pop(*intermediates);
        if (it.ref.type == REF_TEMPORARY && (it.type.connective == TYPE_ARRAY || it.owns_stack_memory)) {
            /* TODO: Combine all of the stack free operations into one? */
            compile_variable_decrements(out, it.ref, &it.type, it.ref_offset, true, true);
        }
    }
}

#endif
