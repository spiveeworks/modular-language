#ifndef MODLANG_EXPRESSIONS_H
#define MODLANG_EXPRESSIONS_H

#include "types.h"

#include "tokenizer.h"

/* This file converts infix expressions into postfix RPN buffers, and also
   compiles RPN buffers into bytecode instructions. In this way we abstract
   over the basic building blocks of builtin operations like `+` to create the
   primitives needed for more complex syntactical constructs. */

/***************************************/
/* Reverse Polish Notation Expressions */
/***************************************/

enum rpn_atom_type {
    RPN_VALUE,
    RPN_UNARY,
    RPN_BINARY,
    RPN_BINARY_REVERSE,
    RPN_GROUPING,
};

struct rpn_atom {
    enum rpn_atom_type type;
    struct token tk;
};

/* This is like our AST, but we will be compiling it as soon as possible. */
struct rpn_buffer {
    struct rpn_atom *data;
    size_t count;
    size_t capacity;
};

/*********************/
/* Expression Parser */
/*********************/

enum precedence_level {
    PRECEDENCE_GROUPING,
    PRECEDENCE_DISJUNCTIVE,
    PRECEDENCE_CONJUNCTIVE,
    PRECEDENCE_COMPARATIVE,
    PRECEDENCE_ADDITIVE,
    PRECEDENCE_MULTIPLICATIVE,
    PRECEDENCE_UNARY,
    PRECEDENCE_STRUCTURAL
};

struct precedence_info {
    enum token_id operator;
    enum precedence_level precedence;
} precedence_info[] = {
    {TOKEN_LOGIC_OR, PRECEDENCE_DISJUNCTIVE},
    {TOKEN_LOGIC_AND, PRECEDENCE_CONJUNCTIVE},
    {TOKEN_EQ, PRECEDENCE_COMPARATIVE},
    {TOKEN_NEQ, PRECEDENCE_COMPARATIVE},
    {TOKEN_LEQ, PRECEDENCE_COMPARATIVE},
    {TOKEN_GEQ, PRECEDENCE_COMPARATIVE},
    {'<', PRECEDENCE_COMPARATIVE},
    {'>', PRECEDENCE_COMPARATIVE},
    {'|', PRECEDENCE_ADDITIVE},
    {'^', PRECEDENCE_ADDITIVE},
    {'+', PRECEDENCE_ADDITIVE},
    {'-', PRECEDENCE_ADDITIVE},
    {TOKEN_LSHIFT, PRECEDENCE_MULTIPLICATIVE},
    {TOKEN_RSHIFT, PRECEDENCE_MULTIPLICATIVE},
    {'&', PRECEDENCE_MULTIPLICATIVE},
    {'*', PRECEDENCE_MULTIPLICATIVE},
    {'/', PRECEDENCE_MULTIPLICATIVE},
    {'%', PRECEDENCE_MULTIPLICATIVE},
    {'.', PRECEDENCE_STRUCTURAL},
};

void push_rpn_ref(struct rpn_buffer *out, struct rpn_ref *ref) {
    if (ref->push) {
        struct rpn_atom it = {RPN_VALUE, ref->tk};
        buffer_push(*out, it);
    }
}

struct partial_operation {
    struct rpn_ref arg; /* May or may not give an RPN_VALUE to push. */
    struct rpn_atom op; /* Can't be RPN_VALUE. */
    enum precedence_level precedence;
};

struct partial_operation_buffer {
    size_t count;
    size_t capacity;
    struct partial_operation *data;
};

struct op_stack {
    /* Represents something like  a || b && c == d +  */
    struct partial_operation_buffer lhs;

    /* Represents the subsequent e, which either groups to the left or to the
       right. */
    bool have_next_ref;
    struct rpn_ref next_ref;

    /* Represents the subsequent operation, which either causes a cascade of
       binary operations to group together, or adds a new partial operation to
       the stack. */
    bool have_next_op;
    struct rpn_atom next_op;
    enum precedence_level next_precedence;

    /* Represents a closing bracket or semicolon that will pop results until
       either an opening bracket is reached, or until the stack is empty. */
    bool have_closing_token;
    enum token_id opening_id;
    struct token closing_token;
};

struct expr_parse_result {
    bool has_ref_decl;
    struct rpn_buffer atoms;
};

struct expr_parse_result parse_expression(struct tokenizer *tokenizer) {
    bool has_ref_decl = false;
    struct rpn_buffer out = {0};

    struct op_stack stack = {0};
    while (true) {
        struct partial_operation *top = buffer_top(stack.lhs);
        /* e.g. ADDITIVE and MULTIPLICATIVE will both pop a MULTIPLICATIVE,
           but neither will pop a COMPARATIVE.

               a * b *        a * b +        a < b *        a < b +

           So pop if the new thing is lower (closer to ||) than the old. */
        bool pop = false;
        if (stack.have_next_ref && stack.have_next_op) {
            pop = top && top->precedence != PRECEDENCE_GROUPING
                && stack.next_precedence <= top->precedence;
        } else if (stack.have_next_ref && stack.have_closing_token) {
            pop = top && top->precedence != PRECEDENCE_GROUPING;
        }
        if (pop) {
            push_rpn_ref(&out, &top->arg);
            push_rpn_ref(&out, &stack.next_ref);
            if (top->arg.push && !stack.next_ref.push) {
                top->op.type = RPN_BINARY_REVERSE;
            }
            buffer_push(out, top->op);

            /* E.g. goes from {a + b *; c; +}, to {a +; b * c; +}. */
            /* or {a * ( b +; c; )}, to {a * (; b * c; )}. */
            stack.next_ref.push = false;
            stack.lhs.count--;
        } else if (stack.have_next_ref && stack.have_closing_token) {
            /* Can't pop any more, so either resolve two brackets, or return
               the final result. */
            if (stack.opening_id == TOKEN_NULL) {
                if (top) {
                    fprintf(stderr, "Error on line %d, %d: Got unexpected "
                        "token \"", stack.closing_token.row,
                        stack.closing_token.column);
                    fputstr(stack.closing_token.it, stderr);
                    fprintf(stderr, "\" while parsing expression.\n");
                    exit(EXIT_FAILURE);
                }

                push_rpn_ref(&out, &stack.next_ref);

                put_token_back(tokenizer, stack.closing_token);

                /* Now finish up. */
                buffer_free(stack.lhs);

                struct expr_parse_result result;
                result.has_ref_decl = has_ref_decl;
                result.atoms = out;

                return result;
            } else if (!top) {
                fprintf(stderr, "Error on line %d, %d: Got unmatched "
                    "bracket \"", stack.closing_token.row, stack.closing_token.column);
                fputstr(stack.closing_token.it, stderr);
                fprintf(stderr, "\" while parsing expression.\n");
                exit(EXIT_FAILURE);
            } else if (stack.opening_id != top->op.tk.id) {
                fprintf(stderr, "Error on line %d, %d: Got incorrectly "
                    "matched brackets \"%c\" and \"%c\" while parsing "
                    "expression.", stack.closing_token.row,
                    stack.closing_token.column, top->op.tk.id,
                    stack.closing_token.id);
                exit(EXIT_FAILURE);

            } else {
                /* Resolve the brackets. */
                stack.lhs.count -= 1;
                stack.have_closing_token = false;
                /* Keep next_ref, as if the brackets had been replaced by a
                   single variable name. */
            }
        } else if (!stack.have_next_ref) {
            struct token tk = get_token(tokenizer);

            if (tk.id == TOKEN_NUMERIC || tk.id == TOKEN_ALPHANUM) {
                stack.next_ref.push = true;
                stack.next_ref.tk = tk;
                stack.have_next_ref = true;
            } else if (tk.id == '(') {
                /* Nothing to cascade, just push the paren and continue. */
                struct partial_operation new;
                new.op.type = RPN_GROUPING;
                new.op.tk = tk;
                new.precedence = PRECEDENCE_GROUPING;
                buffer_push(stack.lhs, new);
            } else {
                fprintf(stderr, "Error on line %d, %d: Got unexpected "
                    "token \"", tk.row, tk.column);
                fputstr(tk.it, stderr);
                fprintf(stderr, "\" while parsing expression.\n");
                exit(EXIT_FAILURE);
            }
        } else if (!stack.have_next_op) {
            struct token tk = get_token(tokenizer);
            enum token_id op = tk.id;

            for (int i = 0; i < ARRAY_LENGTH(precedence_info); i++) {
                if (tk.id == precedence_info[i].operator) {
                    stack.next_op.type = RPN_BINARY;
                    stack.next_op.tk = tk;
                    stack.next_precedence = precedence_info[i].precedence;
                    stack.have_next_op = true;
                    break;
                }
            }
            if (!stack.have_next_op) {
                stack.have_closing_token = true;
                stack.closing_token = tk;
                if (tk.id == ')') stack.opening_id = '(';
                else stack.opening_id = TOKEN_NULL;
            }
        } else {
            /* We have a ref and an operation, and they didn't cause anything
               to pop, so push instead, and try again. */
            struct partial_operation new;
            new.arg = stack.next_ref;
            new.op = stack.next_op;
            new.precedence = stack.next_precedence;
            buffer_push(stack.lhs, new);

            stack.have_next_ref = false;
            stack.have_next_op = false;

            /* Continue through the loop again. */
        }
    }
}

/***********************/
/* Expression Compiler */
/***********************/

void compile_expression(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct type_buffer *intermediates,
    struct rpn_buffer *in
) {
    int i = 0;
    while (i < in->count) {
        int j;
        bool have_op = false;
        enum rpn_atom_type type;
        for (j = 0; j < 3 && i + j < in->count; j++) {
            type = in->data[i + j].type;
            if (type == RPN_BINARY
                || type == RPN_BINARY_REVERSE
                || (j < 2 && type == RPN_UNARY))
            {
                have_op = true;
                break;
            } else if (type != RPN_VALUE) {
                /* make sure have_op is false, if we get some grouping operator
                   before any binary operators. */
                break;
            }
        }
        if (!have_op) {
            j = 0;
            type = in->data[i].type;
        }

        if (have_op) {
            struct rpn_ref arg1;
            struct rpn_ref arg2;
            if (j >= 1) {
                arg2.push = true;
                arg2.tk = in->data[i + j - 1].tk;
            } else {
                arg2.push = false;
            }
            if (j >= 2) {
                arg1.push = true;
                arg1.tk = in->data[i + j - 2].tk;
            } else {
                arg1.push = false;
            }
            if (type == RPN_UNARY) {
                fprintf(stderr, "Error: Unary operators are not yet "
                    "implemented.\n");
                exit(EXIT_FAILURE);
            } if (type == RPN_BINARY) {
                compile_operation(
                    out,
                    bindings,
                    intermediates,
                    &arg1,
                    &arg2,
                    in->data[i + j].tk
                );
            } else if (type == RPN_BINARY_REVERSE) {
                compile_operation(
                    out,
                    bindings,
                    intermediates,
                    &arg2,
                    &arg1,
                    in->data[i + j].tk
                );
            } else {
                fprintf(stderr, "Error: have_op without type that is an "
                    "op?\n");
                exit(EXIT_FAILURE);
            }
        } else if (type == RPN_VALUE) {
            struct ref ref = compile_value_token(bindings, &in->data[i].tk);
            if (ref.type == REF_LOCAL) {
                struct type *binding_type = &bindings->data[ref.x].type;
                buffer_push(*intermediates, *binding_type);
                if (binding_type->connective != TYPE_INT || binding_type->size != 3) {
                    fprintf(stderr, "Error: Move instructions are only "
                        "implemented for 64 bit integers.\n");
                    exit(EXIT_FAILURE);
                }
            } else if (ref.type == REF_CONSTANT) {
                struct type output_type;
                output_type.connective = TYPE_INT;
                output_type.size = 3;
                buffer_push(*intermediates, output_type);
            } else {
                fprintf(stderr, "Error: Got unknown ref type during mov "
                    "compilation?\n");
                exit(EXIT_FAILURE);
            }

            struct instruction instr;
            instr.op = OP_MOV;
            instr.flags = OP_64BIT;
            instr.output.type = REF_TEMPORARY;
            instr.output.x = intermediates->count - 1;
            instr.arg1 = ref;
            instr.arg2.type = REF_NULL;
            buffer_push(*out, instr);
        } else {
            fprintf(stderr, "Error: Got unknown atom type in RPN "
                "compilation?\n");
            exit(EXIT_FAILURE);
        }

        i = i + j + 1;
    }
}

void assert_match_pattern(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct rpn_buffer *pattern,
    struct type_buffer *values
) {
    /* TODO: do I want to bind these forwards? Or backwards? */
    while (pattern->count > 0) {
        if (values->count == 0) {
            struct token *tk = &pattern->data[0].tk;
            fprintf(stderr, "Error at line %d, %d: There are more values on "
                "the left hand side of the assignment than on the right hand "
                "side.\n", tk->row, tk->column);
            exit(EXIT_FAILURE);
        }

        struct rpn_atom *name = buffer_top(*pattern);
        if (name->type != RPN_VALUE) {
            fprintf(stderr, "Error at line %d, %d: The operator \"",
                name->tk.row, name->tk.column);
            fputstr(name->tk.it, stderr);
            fprintf(stderr, "\" appeared on the left hand side of an "
                "assignment statement. Pattern matching is not "
                "implemented.\n");
            exit(EXIT_FAILURE);
        }
        if (name->tk.id != TOKEN_ALPHANUM) {
            fprintf(stderr, "Error at line %d, %d: The literal \"",
                name->tk.row, name->tk.column);
            fputstr(name->tk.it, stderr);
            fprintf(stderr, "\" appeared on the left hand side of an "
                "assignment statement. Pattern matching is not "
                "implemented.\n");
            exit(EXIT_FAILURE);
        }

        struct record_entry *new = buffer_addn(*bindings, 1);
        new->name = name->tk.it;
        struct type val_type = buffer_pop(*values);
        new->type = val_type;

        /* TODO: Make a procedure for these mov operations. */
        struct instruction instr;
        instr.op = OP_MOV;
        instr.flags = OP_64BIT;
        instr.output.type = REF_LOCAL;
        instr.output.x = bindings->count - 1;
        instr.arg1.type = REF_TEMPORARY;
        instr.arg1.x = values->count;
        instr.arg2.type = REF_NULL;
        buffer_push(*out, instr);

        if (pattern->count == 1 && values->count > 0) {
            struct token *tk = &pattern->data[0].tk;
            fprintf(stderr, "Error at line %d, %d: There are more values on "
                "the right hand side of the assignment than on the left hand "
                "side.\n", tk->row, tk->column);
            exit(EXIT_FAILURE);
        }

        pattern->count -= 1;
    }
}

#endif
