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

/* The spirit of the parser is USUALLY to follow a naive postfix approach for
   representing everything. calculate intermediate values using constants and
   variables, push them onto the stack, crush them down to new intermediate
   values using other operations, function calls, etc. and finally write the
   result to named variable. The exception to this is in situations where we
   need hints from prefix parsing in order to make compilation easier or more
   powerful; in these situations we might add atoms _before_ some intermediate
   values are calculated, so that some work can be done before any of those
   intermediate values get compiled, e.g. to allocate an array before filling
   it with values. Note also that we defer pushing constants and variables on
   until just before they are consumed by an operator, which sometimes requires
   reversing a binary operator, so that e.g. a constant can be divided by a
   complex subexpression. We shouldn't need to implement this deferral during
   parsing, though! TODO */

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
    int multi_value_count;
    /* bool is_postfix_grouping; or something, for f(x) and arr[i], etc. or
       maybe this should be another rpn_atom_type? */
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
    /* Mutually exclusive with have_next_op. */
    bool have_closing_token;
    enum token_id opening_id;
    struct token closing_token;
};

struct expr_parse_result {
    bool has_ref_decl;
    struct rpn_buffer atoms;
    int multi_value_count;
};

/* TODO: make the prefix -> infix -> prefix -> infix structure more clear by
   pulling prefix parsing and infix parsing out into separate procedures.
   From a code theory point of view, they would still be constraint-bearing
   code, but they bear different kinds of constraints, and pulling them out
   says "Hey! There's a different kind of thing happening here!"... More to the
   point, though, it is a state machine, so the usual spaghetti cost of pulling
   out functions is basically nonexistent anyway, since it is already FSM
   spaghetti. */
struct expr_parse_result parse_expression(struct tokenizer *tokenizer) {
    bool has_ref_decl = false;
    struct rpn_buffer out = {0};
    int final_multi_value_count = 0;

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
            /* or {a * ( b +; c; )}, to {a * (; b + c; )}. */
            stack.next_ref.push = false;
            stack.lhs.count--;
        } else if (stack.have_next_ref && stack.have_closing_token) {
            /* Can't pop any more, but have hit a delimiter or comma or
               something, so handle the closing bracket or try and return a
               final result or something. */
            if (stack.closing_token.id == ',') {
                push_rpn_ref(&out, &stack.next_ref);
                stack.have_next_ref = false;
                if (top) {
                    if (top->precedence != PRECEDENCE_GROUPING) {
                        /* Should be impossible, but check anyway. */
                        fprintf(stderr, "Error: Hit a comma, and tried to "
                            "push a value into a non-grouping token?\n");
                        exit(EXIT_FAILURE);
                    }
                    top->op.multi_value_count += 1;

                    if (top->op.tk.id == '(') {
                        fprintf(stderr, "Error at line %d, %d: There was a "
                            "comma inside grouping parentheses.\n",
                            top->op.tk.row, top->op.tk.column);
                        exit(EXIT_FAILURE);
                    }

                    /* Take the result that was just calculated, and write it
                       into whatever array or struct is being built. This is
                       denoted with a single comma token in the RPN buffer. */
                    struct rpn_atom *comma = buffer_addn(out, 1);
                    comma->type = RPN_GROUPING;
                    comma->tk = stack.closing_token;
                } else {
                    /* Just let the cascaded result exist on the stack. */
                    final_multi_value_count += 1;
                }

                stack.have_closing_token = false;
            } else if (stack.opening_id == TOKEN_NULL) {
                if (top) {
                    fprintf(stderr, "Error on line %d, %d: Got unexpected "
                        "token \"", stack.closing_token.row,
                        stack.closing_token.column);
                    fputstr(stack.closing_token.it, stderr);
                    fprintf(stderr, "\" while parsing expression.\n");
                    exit(EXIT_FAILURE);
                }

                push_rpn_ref(&out, &stack.next_ref);
                final_multi_value_count += 1;

                put_token_back(tokenizer, stack.closing_token);

                /* Now finish up. */
                buffer_free(stack.lhs);

                struct expr_parse_result result;
                result.has_ref_decl = has_ref_decl;
                result.atoms = out;
                result.multi_value_count = final_multi_value_count;

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
            } else if (stack.closing_token.id == ')') {
                /* Resolve the brackets. */
                stack.lhs.count -= 1;
                stack.have_closing_token = false;
                /* Keep next_ref, as if the brackets had been replaced by a
                   single variable name. */
            } else {
                /* Push the value with one last comma, as if it were written
                   manually. */
                push_rpn_ref(&out, &stack.next_ref);
                struct rpn_atom comma = {0};
                comma.type = RPN_GROUPING;
                comma.tk.id = ',';
                buffer_push(out, comma);
                top->op.multi_value_count += 1;

                struct rpn_atom close;
                close.type = RPN_GROUPING;
                close.tk = stack.closing_token;
                close.multi_value_count = top->op.multi_value_count;
                buffer_push(out, close);

                stack.next_ref.push = false;
                stack.have_next_ref = true;

                stack.lhs.count--;
                stack.have_closing_token = false;
            }
        } else if (!stack.have_next_ref) {
            struct token tk = get_token(tokenizer);

            if (tk.id == TOKEN_NUMERIC || tk.id == TOKEN_ALPHANUM) {
                stack.next_ref.push = true;
                stack.next_ref.tk = tk;
                stack.have_next_ref = true;
            } else if (tk.id == '(' || tk.id == '[' || tk.id == '{') {
                /* Nothing to cascade, just push the paren and continue. */
                struct partial_operation new;
                new.op.type = RPN_GROUPING;
                new.op.tk = tk;
                new.op.multi_value_count = 0;
                new.precedence = PRECEDENCE_GROUPING;
                buffer_push(stack.lhs, new);

                if (tk.id != '(') {
                    /* for '[' and '{' we actually want to store an atom in the
                       rpn_buffer straight away, so that the compiler can make
                       an instruction to allocate memory up front. */
                    buffer_push(out, new.op);
                }
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
                else if (tk.id == ']') stack.opening_id = '[';
                else if (tk.id == '}') stack.opening_id = '{';
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

struct emplace_info {
    size_t alloc_instruction_index;
    size_t pointer_variable_index; /* The temp/intermediate index of the output
                                      pointer. */
    int multi_value_count;
    int size; /* Total size for structs, per-element size for arrays. */
    bool is_array;
    struct type *element_type;
};

struct emplace_stack {
    struct emplace_info *data;
    size_t count;
    size_t capacity;
};

void compile_expression(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct type_buffer *intermediates,
    struct rpn_buffer *in
) {
    struct emplace_stack emplace_stack = {0};

    int i = 0;
    while (i < in->count) {
        int j;
        bool have_op = false;
        enum rpn_atom_type type;
        /* look ahead for patterns like 1 1 +, x y *, y *, etc. */
        /* We have to defer putting atoms in the buffer so that this lookahead
           becomes possible, because in all other situations a value atom means
           PUSH A COPY TO THE CALL STACK, to call a function or procedure. That
           said, this might change in the future, if we want to optimize things
           like `var x := 1;` to a single instruction with no temporaries, or
           similar for expressions like [x, y], then ONLY function calls
           require these temporaries. Yes, this is totally the wrong way right
           now, I should be storing a stack of references, like I did in
           previous RPN shunting yard parsers. TODO */
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
            enum operation_flags flags = 0;

            struct ref ref = compile_value_token(bindings, &in->data[i].tk);
            if (ref.type == REF_GLOBAL) {
                struct type *binding_type = &bindings->data[ref.x].type;
                buffer_push(*intermediates, *binding_type);
                if (binding_type->connective == TYPE_INT) {
                    if (binding_type->word_size != 3) {
                        fprintf(stderr, "Error: Move instructions are only "
                            "implemented for 64 bit integers.\n");
                        exit(EXIT_FAILURE);
                    }

                    flags = OP_64BIT;
                } else if (binding_type->connective == TYPE_ARRAY) {
                    flags = OP_SHARED_BUFF;
                }
            } else if (ref.type == REF_CONSTANT) {
                buffer_push(*intermediates, type_int64);
                flags = OP_64BIT;
            } else {
                fprintf(stderr, "Error: Got unknown ref type during mov "
                    "compilation?\n");
                exit(EXIT_FAILURE);
            }

            struct instruction instr;
            instr.op = OP_MOV;
            instr.flags = flags;
            instr.output.type = REF_TEMPORARY;
            instr.output.x = intermediates->count - 1;
            instr.arg1 = ref;
            instr.arg2.type = REF_NULL;
            buffer_push(*out, instr);
        } else if (type != RPN_GROUPING) {
            fprintf(stderr, "Error: Got unknown atom type in RPN "
                "compilation?\n");
            exit(EXIT_FAILURE);
        } else if (in->data[i].tk.id == '[') {
            struct emplace_info *next_emplace = buffer_addn(emplace_stack, 1);
            next_emplace->alloc_instruction_index = out->count;
            buffer_addn(*out, 1);
            next_emplace->multi_value_count = 0;
            next_emplace->size = 0;
            next_emplace->is_array = true;

            /* TODO: how do I handle these array types?? What kinds of type
               inference am I planning on having? */
            next_emplace->pointer_variable_index = intermediates->count;
            struct type array_type = type_array_of(type_int64);
            buffer_push(*intermediates, array_type);
        } else if (in->data[i].tk.id == '{') {
            fprintf(stderr, "Error: Struct literals are not yet "
                "implemented.\n");
            exit(EXIT_FAILURE);
        } else if (in->data[i].tk.id == ',') {
            struct emplace_info *em = buffer_top(emplace_stack);
            if (!em) {
                fprintf(stderr, "Error at line %d, %d: Multi-values are not "
                    "yet implemented.\n", in->data[i].tk.row,
                    in->data[i].tk.column);
                exit(EXIT_FAILURE);
            }
            struct type *ty = buffer_top(*intermediates);
            if (em->multi_value_count == 0) {
                em->size = ty->total_size;
                em->element_type = ty;
            } else {
                /* TODO: properly compare types to make sure the elements of
                   the array all agree */
                if (em->size != ty->total_size) {
                    fprintf(stderr, "Error at line %d, %d: Array elements had "
                        "different sizes.\n", in->data[i].tk.row,
                        in->data[i].tk.column);
                    exit(EXIT_FAILURE);
                }
            }
            struct instruction instr;
            instr.op = OP_ARRAY_STORE;
            instr.flags = OP_64BIT;
            instr.output.type = REF_TEMPORARY;
            instr.output.x = em->pointer_variable_index;
            instr.arg1.type = REF_CONSTANT;
            instr.arg1.x = em->multi_value_count;
            /* TODO: rearrange this whole thing to store refs in a stack, and
               pop arg2 from that. */
            instr.arg2.type = REF_TEMPORARY;
            instr.arg2.x = intermediates->count - 1;
            buffer_push(*out, instr);
            intermediates->count -= 1;
            em->multi_value_count += 1;
        } else if (in->data[i].tk.id == ']') {
            if (emplace_stack.count <= 0) {
                fprintf(stderr, "Error: Tried to compile unmatched close "
                    "bracket?\n");
                exit(EXIT_FAILURE);
            }
            struct emplace_info em = buffer_pop(emplace_stack);

            struct instruction *alloc_instr =
                &out->data[em.alloc_instruction_index];
            alloc_instr->op = OP_ARRAY_ALLOC;
            alloc_instr->flags = 0;
            alloc_instr->output.type = REF_TEMPORARY;
            alloc_instr->output.x = em.pointer_variable_index;
            alloc_instr->arg1.type = REF_STATIC_POINTER;
            alloc_instr->arg1.x = (int64)em.element_type;
            alloc_instr->arg2.type = REF_CONSTANT;
            alloc_instr->arg2.x = em.multi_value_count;
        } else if (in->data[i].tk.id == '}') {
            fprintf(stderr, "Error: Struct literals are not yet "
                "implemented.\n");
            exit(EXIT_FAILURE);
        } else {
            fprintf(stderr, "Error at line %d, %d: Unknown/unimplemented "
                "grouping token '", in->data[i].tk.row, in->data[i].tk.column);
            fputstr(in->data[i].tk.it, stderr);
            fprintf(stderr, "'\n");
            exit(EXIT_FAILURE);
        }

        i = i + j + 1;
    }

    buffer_free(emplace_stack);
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
        /* TODO: make this local if we are in a block or a function. */
        struct ref new_var = {REF_GLOBAL, bindings->count - 1};
        bindings->global_count = bindings->count;

        /* TODO: Make a procedure for these mov operations. */
        struct instruction instr;
        instr.op = OP_MOV;
        if (val_type.connective == TYPE_INT) {
            if (val_type.word_size != 3) {
                fprintf(stderr, "Error: Assignment is not currently supported "
                    "for scalars other than int64.\n");
                exit(EXIT_FAILURE);
            }
            instr.flags = OP_64BIT;
        } else if (val_type.connective == TYPE_ARRAY) {
            instr.flags = OP_SHARED_BUFF;
        }
        instr.output = new_var;
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
