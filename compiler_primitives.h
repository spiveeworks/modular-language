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

struct type get_type_info(
    struct record_table *bindings,
    struct type_buffer *intermediates,
    struct ref it
) {
    struct type result;
    switch (it.type) {
      case REF_CONSTANT:
        result = type_int64;
        break;
      case REF_GLOBAL:
        result = bindings->data[it.x].type;
        break;
      case REF_LOCAL:
        result = bindings->data[bindings->global_count + it.x].type;
        break;
      case REF_TEMPORARY:
        result = intermediates->data[it.x];
        break;
      default:
        fprintf(stderr, "Error: Tried to infer type info from a ref with ref "
            "type %d?\n", it.type);
        exit(EXIT_FAILURE);
    }
    return result;
}

struct ref compile_value_token(
    struct record_table *bindings,
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

        enum ref_type type;
        if (ind < bindings->global_count) {
            type = REF_GLOBAL;
        } else {
            type = REF_LOCAL;
            ind -= bindings->global_count;
        }
        return (struct ref){type, ind};
    }
    /* else */
    if (in->id == TOKEN_NUMERIC) {
        int64 value = integer_from_string(in->it);
        return (struct ref){REF_CONSTANT, value};
    }
    /* else */

    fprintf(stderr, "Error: Asked to compile \"");
    fputstr(in->it, stderr);
    fprintf(stderr, "\" as an RPN atom?\n");
    exit(EXIT_FAILURE);
}

/* Really more of an expression concept than an operator concept, but this
   represents an argument to an operation that can either be a temporary, or an
   explicit token. */
struct rpn_ref {
    /* Could use TOKEN_NULL instead of this flag. */
    bool push;
    struct token tk;
};

void compile_operation(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct type_buffer *intermediates,
    struct rpn_ref *arg1,
    struct rpn_ref *arg2,
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

    struct instruction result;
    result.op = op->opcode;

    int intermediate_count = intermediates->count;
    if (arg2->push) {
        result.arg2 = compile_value_token(bindings, &arg2->tk);
    } else {
        intermediate_count--;

        result.arg2.type = REF_TEMPORARY;
        result.arg2.x = intermediate_count;
    }
    if (arg1->push) {
        result.arg1 = compile_value_token(bindings, &arg1->tk);
    } else {
        intermediate_count--;

        result.arg1.type = REF_TEMPORARY;
        result.arg1.x = intermediate_count;
    }
    if (intermediate_count < 0) {
        fprintf(stderr, "Error: Ran out of temporaries??\n");
        exit(EXIT_FAILURE);
    }

    /* Maybe make a combined rpn_ref -> {ref, type} compile function? */
    struct type arg1_type =
        get_type_info(bindings, intermediates, result.arg1);
    struct type arg2_type =
        get_type_info(bindings, intermediates, result.arg2);

    struct type result_type = type_int64;

    if (op->opcode == OP_ARRAY_INDEX) {
        /* Casing all the scalar connectives sounds annoying. Maybe I should
           make the connective "scalar", or work out some bit mask trick to
           test them all in one go. */
        if (arg1_type.connective != TYPE_ARRAY) {
            fprintf(stderr, "Error: Left side of array index must be an "
                "array.\n");
            exit(EXIT_FAILURE);
        }
        if (arg2_type.connective != TYPE_INT) {
            fprintf(stderr, "Error: Array index must be an integer.\n");
            exit(EXIT_FAILURE);
        }
        if (arg2_type.word_size != 3) {
            fprintf(stderr, "Error: Currently only 64 bit integer types are "
                    "implemented.\n");
            exit(EXIT_FAILURE);
        }
        result.flags = OP_64BIT;

        result_type = *arg1_type.inner;
    } else if (op->opcode == OP_ARRAY_CONCAT) {
        if (arg1_type.connective != TYPE_ARRAY || arg2_type.connective != TYPE_ARRAY) {
            fprintf(stderr, "Error: Arguments to ++ operator must be "
                "arrays.\n");
            exit(EXIT_FAILURE);
        }
        /* TODO: check the array types agree. */
        fprintf(stderr, "Warning: Currently array concat is not type "
            "checked.\n");

        result_type = arg1_type;
    } else {
        /* Casing all the scalar connectives sounds annoying. Maybe I should
           make the connective "scalar", or work out some bit mask trick to
           test them all in one go. */
        if (arg1_type.connective != TYPE_INT || arg2_type.connective != TYPE_INT) {
            fprintf(stderr, "Error: Argument to operator %c must be an integer.\n",
                    operation.id);
            exit(EXIT_FAILURE);
        }
        if (arg1_type.word_size != 3 || arg2_type.word_size != 3) {
            fprintf(stderr, "Error: Currently only 64 bit integer types are "
                    "implemented.\n");
            exit(EXIT_FAILURE);
        }
        result.flags = OP_64BIT;
    }

    result.output.type = REF_TEMPORARY;
    result.output.x = intermediate_count;

    buffer_push(*out, result);

    while (intermediates->count > intermediate_count) {
        /* We should be managing these intermediate types, but they get shared
           by emplace logic and thus by array_alloc instructions, so really
           they need to be shared, or deep cloned each time. Shared probably
           makes sense, but then what is the lifetime for the code that we
           generate?? Indefinite? */
        /* destroy_type(buffer_top(*intermediates)); */
        intermediates->count -= 1;
    }

    buffer_push(*intermediates, result_type);
}

#endif
