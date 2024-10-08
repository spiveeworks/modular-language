#ifndef MODLANG_EXPRESSIONS_H
#define MODLANG_EXPRESSIONS_H

#include "types.h"

#include "tokenizer.h"
#include "compiler_primitives.h"

/* This file converts infix expressions into postfix RPN-ish buffers, and also
   compiles RPN buffers into bytecode instructions. This builds on
   compiler_primitives.h to build complex syntactical expressions, but provides
   no concept of statements, assignment, or statement-based control flow. */

/********************/
/* Pattern Commands */
/********************/

enum pattern_command_type {
    PATTERN_DECL,
    PATTERN_VALUE,

    PATTERN_UNARY,
    PATTERN_BINARY,
    PATTERN_MEMBER,

    PATTERN_PROCEDURE_CALL,
    PATTERN_ARRAY,
    PATTERN_STRUCT,

    PATTERN_END_ARG,
    PATTERN_END_TERM
};

struct pattern_command {
    enum pattern_command_type type;

    bool is_var;
    bool takes_ref; /* What was this for, again? */

    struct token tk;
    struct token identifier; /* For record literals and maybe other things. */

    int arg_count;
    size_t arg_command_count;

    bool has_child_struct;
};

/* This is like our AST, but we will be compiling it as soon as possible. */
struct pattern {
    struct pattern_command *data;
    size_t count;
    size_t capacity;

    int multi_value_count;
    bool valid_pattern;
    bool valid_expression;
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
    PRECEDENCE_UNARY
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
    {TOKEN_CONCAT, PRECEDENCE_ADDITIVE},
    {TOKEN_LSHIFT, PRECEDENCE_MULTIPLICATIVE},
    {TOKEN_RSHIFT, PRECEDENCE_MULTIPLICATIVE},
    {'&', PRECEDENCE_MULTIPLICATIVE},
    {'*', PRECEDENCE_MULTIPLICATIVE},
    {'/', PRECEDENCE_MULTIPLICATIVE},
    {'%', PRECEDENCE_MULTIPLICATIVE}
};

enum partial_operation_type {
    PARTIAL_BINARY,

    PARTIAL_PAREN,

    PARTIAL_INDEX,
    PARTIAL_PROCEDURE_CALL,
    PARTIAL_ARRAY,
    PARTIAL_TUPLE,
    PARTIAL_RECORD,
    PARTIAL_FIELD,
};

/* A pattern_command that is still accumulating inputs. */
struct partial_operation {
    enum partial_operation_type type;
    enum precedence_level precedence;

    bool takes_ref;

    struct token op;

    int arg_count;
    size_t arg_command_count;
    size_t open_command_index;

    size_t has_child_struct;
};

struct partial_operation_buffer {
    size_t count;
    size_t capacity;
    struct partial_operation *data;
};

struct op_stack {
    /* If we have parsed something like a || b && c == d + e
       then the RPN output will be `a b c d e`, but we won't know whether to
       push `+` to the stack until we see what the next token is. */
    struct partial_operation_buffer lhs;
    /* This is the number of open brackets are in the parse stack, minus one if
       have_closing_token is trying to close a bracket. This lets us detect
       complete expressions in REPL mode. */
    int grouping_count;

    /* As we parse either we are waiting for another variable/literal/open
       bracket, or we are waiting for another binary operation/close paren, or
       we have both and need to unwind and output some operators into the RPN
       buffer. have_next_ref represents whether we have the variable/literal
       yet. */
    bool have_next_ref;

    /* The subsequent operation that we want to put on the stack, which either
       causes a cascade of higher precedence binary operations to be emitted,
       or eventually gets added to the stack. */
    bool have_next_op;
    struct token next_op;
    enum precedence_level next_precedence;

    /* Represents a closing bracket or semicolon that will pop results until
       either an opening bracket is reached, or until the stack is empty. */
    /* Mutually exclusive with have_next_op. */
    bool have_closing_token;
    enum token_id opening_id;
    struct token closing_token;
};

/* The basic heart beat of expression parsing: roughly every second token is in
   "ref" position, usually either a literal or a variable, or an open
   delimiter, and in between those are "op" position tokens, usually infix or
   postfix operations, or close delimiters. Both of these are fairly simple
   changes to the op stack/state machine, but then when close delimiters are
   reached, and *finish* resolving down to a single subexpression, then a third
   procedure comes in, the monster `resolve_closing_token` procedure. */
void read_next_ref(
    struct tokenizer *tokenizer,
    struct op_stack *stack,
    struct pattern *out
) {
    struct token tk = get_token(tokenizer);

    if (tk.id == TOKEN_VAR) {
        tk = get_token(tokenizer);
        if (tk.id != TOKEN_ALPHANUM) {
            fprintf(stderr, "Error on line %d, %d: Got unexpected "
                    "token \"", tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" while parsing var declaration.\n");
            exit(EXIT_FAILURE);
        }

        struct pattern_command val = {PATTERN_VALUE};
        val.tk = tk;
        val.is_var = true;
        buffer_push(*out, val);
        stack->have_next_ref = true;
    } else if (tk.id == TOKEN_NUMERIC || tk.id == TOKEN_ALPHANUM) {
        /* In record literals we don't want to interpret names as variables, so
           peek to see if there is a colon and if it is part of a record
           literal. This code path will also get activated by type ascriptions
           in patterns, so we could handle that too one day. */
        struct token next_tk = get_token(tokenizer);
        if (next_tk.id == ':') {
            struct partial_operation *top = buffer_top(stack->lhs);
            if (top && top->type == PARTIAL_TUPLE) {
                if (top->arg_count != 0) {
                    fprintf(stderr, "Error at line %d, %d: Got ':' token inside a "
                        "tuple expression.\n", next_tk.row, next_tk.column);
                    exit(EXIT_FAILURE);
                }
                top->type = PARTIAL_RECORD;
            }
            if (!top || top->type != PARTIAL_RECORD) {
                fprintf(stderr, "Error at line %d, %d: Got ':' token that wasn't "
                    "in a record literal or wasn't in the correct location.\n",
                    next_tk.row, next_tk.column);
                exit(EXIT_FAILURE);
            }
            struct partial_operation new = {PARTIAL_FIELD, PRECEDENCE_GROUPING};
            new.op = tk; /* TODO: Are there situations where I want to store
                            two tokens, one for compilation and the other for
                            error reporting? Well, this is another one of
                            them. */
            buffer_push(stack->lhs, new);
        } else {
            put_token_back(tokenizer, next_tk);
            /* Standard ref position token, emit it and proceed to the cascade
               part of the loop */
            struct pattern_command val = {PATTERN_VALUE};
            val.tk = tk;
            buffer_push(*out, val);
            stack->have_next_ref = true;
        }
    } else if (tk.id == '(') {
        /* Nothing to cascade, just push the paren and continue. */
        struct partial_operation new = {PARTIAL_PAREN, PRECEDENCE_GROUPING};
        new.op = tk; /* For errors, I guess. */
        buffer_push(stack->lhs, new);
        stack->grouping_count += 1;
    } else if (tk.id == '[') {
        /* Nothing to cascade, just push the bracket and continue. */
        struct partial_operation new = {PARTIAL_ARRAY, PRECEDENCE_GROUPING};
        new.op = tk; /* For errors, I guess. */
        new.open_command_index = out->count;
        buffer_push(stack->lhs, new);
        stack->grouping_count += 1;

        struct pattern_command command = {PATTERN_ARRAY};
        command.tk = tk;
        /* for '[' and '{' we actually want to store a command in the buffer
           straight away, so that the compiler can make an instruction to
           allocate memory up front. */
        buffer_push(*out, command);
    } else if (tk.id == '{') {
        /* Nothing to cascade, just push the brace and continue. */
        struct partial_operation new = {PARTIAL_TUPLE, PRECEDENCE_GROUPING};
        new.op = tk; /* For errors, I guess. */
        new.open_command_index = out->count;
        buffer_push(stack->lhs, new);
        stack->grouping_count += 1;

        struct pattern_command command = {PATTERN_STRUCT};
        command.tk = tk;
        /* for '[' and '{' we actually want to store a command in the buffer
           straight away, so that the compiler can make an instruction to
           allocate memory up front. */
        buffer_push(*out, command);
    } else {
        /* We MUST get a ref if we are at the start of an expression, or if we
           just got an infix operator. Anything else is therefore an error. */
        fprintf(stderr, "Error on line %d, %d: Got unexpected "
                "token \"", tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" while parsing expression.\n");
        exit(EXIT_FAILURE);
    }
}

void read_next_op(
    struct tokenizer *tokenizer,
    struct op_stack *stack,
    struct pattern *out
) {
    struct token tk = get_token(tokenizer);

    /* dot operator, put the next token straight into a postfix type pattern
       command, so that we don't try looking it up as a variable. */
    if (tk.id == '.') {
        tk = get_token(tokenizer);
        if (tk.id != TOKEN_ALPHANUM && tk.id != TOKEN_NUMERIC) {
            fprintf(stderr, "Error at line %d, %d: After a dot operator we "
                "expect an identifier or an integer, but instead we got \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\".\n");
        }

        struct pattern_command command = {PATTERN_MEMBER};
        command.tk = tk;
        buffer_push(*out, command);

        return;
    }

    /* infix operators */
    for (int i = 0; i < ARRAY_LENGTH(precedence_info); i++) {
        if (tk.id == precedence_info[i].operator) {
            stack->next_op = tk;
            stack->next_precedence = precedence_info[i].precedence;
            stack->have_next_op = true;
            return;
        }
    }

    /* postfix delimiters */
    if (tk.id == '[') {
        struct partial_operation new = {PARTIAL_INDEX, PRECEDENCE_GROUPING};
        new.op = tk; /* For errors, I guess. */
        buffer_push(stack->lhs, new);
        stack->grouping_count += 1;

        stack->have_next_ref = false;

        /* Still have no op. Proceed to ref position. */
        return;
    } else if (tk.id == '(') {
        struct partial_operation new = {PARTIAL_PROCEDURE_CALL, PRECEDENCE_GROUPING};
        new.op = tk; /* For errors, I guess. */
        new.open_command_index = out->count;
        buffer_push(stack->lhs, new);
        stack->grouping_count += 1;

        stack->have_next_ref = false;

        /* Not sure how to handle function overloads with this approach... May
           need to go back to deferring value compilation, haha. */
        struct pattern_command command = {PATTERN_PROCEDURE_CALL};
        command.tk = tk;
        buffer_push(*out, command);

        /* Still have no op. Proceed to ref position. */
        return;
    }

    /* either a closing delimiter, or the end of the expression */
    stack->have_closing_token = true;
    stack->closing_token = tk;
    if (tk.id == ')') stack->opening_id = '(';
    else if (tk.id == ']') stack->opening_id = '[';
    else if (tk.id == '}') stack->opening_id = '{';
    else stack->opening_id = TOKEN_NULL;

    if (stack->opening_id != TOKEN_NULL) stack->grouping_count -= 1;
}

void op_stack_resolve_arg(
    struct op_stack *stack,
    struct pattern *out
) {
    struct partial_operation *top = buffer_top(stack->lhs);
    if (top) {
        if (top->type == PARTIAL_FIELD) {
            struct pattern_command comma = {PATTERN_END_ARG};
            comma.tk = stack->closing_token;
            comma.identifier = top->op;
            buffer_push(*out, comma);

            buffer_pop(stack->lhs);
            top = buffer_top(stack->lhs);
            if (!top) {
                fprintf(stderr, "Error: Got record partial command that "
                    "wasn't attached to a struct partial command?\n");
                exit(EXIT_FAILURE);
            }

            top->arg_count += 1;
        } else {
            if (top->precedence != PRECEDENCE_GROUPING) {
                /* Should be impossible, but check anyway. */
                fprintf(stderr, "Error: Hit a comma, and tried to "
                        "push a value into a non-grouping token?\n");
                exit(EXIT_FAILURE);
            }
            top->arg_count += 1;

            if (top->type == PARTIAL_PAREN) {
                fprintf(stderr, "Error at line %d, %d: There was a "
                        "comma inside grouping parentheses.\n",
                        top->op.row, top->op.column);
                exit(EXIT_FAILURE);
            }

            /* Take the result that was just calculated, and write it
               into whatever array or struct is being built. This is
               denoted with a single comma token in the RPN buffer. */
            struct pattern_command comma = {PATTERN_END_ARG};
            comma.tk = stack->closing_token;
            buffer_push(*out, comma);
        }
    } else {
        struct pattern_command comma = {PATTERN_END_TERM};
        comma.tk = stack->closing_token;
        buffer_push(*out, comma);
        /* Just let the cascaded result exist on the stack. */
        out->multi_value_count += 1;
    }
}

bool resolve_closing_token(
    struct op_stack *stack,
    struct pattern *out
) {
    struct partial_operation *top = buffer_top(stack->lhs);
    /* Can't pop any more, but have hit a delimiter or comma or
       something, so handle the closing bracket or try and return a
       final result or something. */
    if (stack->closing_token.id == ',') {
        op_stack_resolve_arg(stack, out);

        stack->have_next_ref = false;
        stack->have_closing_token = false;
    } else if (stack->opening_id == TOKEN_NULL) {
        if (top) {
            fprintf(stderr, "Error on line %d, %d: Got unexpected "
                    "token \"", stack->closing_token.row,
                    stack->closing_token.column);
            fputstr(stack->closing_token.it, stderr);
            fprintf(stderr, "\" while parsing expression.\n");
            exit(EXIT_FAILURE);
        }

        out->multi_value_count += 1;

        return true;
    } else if (!top) {
        fprintf(stderr, "Error on line %d, %d: Got unmatched "
                "bracket \"", stack->closing_token.row, stack->closing_token.column);
        fputstr(stack->closing_token.it, stderr);
        fprintf(stderr, "\" while parsing expression.\n");
        exit(EXIT_FAILURE);
    } else if (top->type == PARTIAL_FIELD && stack->opening_id != '{') {
        fprintf(stderr, "Error on line %d, %d: Got incorrectly "
                "matched brackets \"{\" and \"%c\" while parsing "
                "expression.", stack->closing_token.row,
                stack->closing_token.column, stack->closing_token.id);
        exit(EXIT_FAILURE);
    } else if (top->type != PARTIAL_FIELD && stack->opening_id != top->op.id) {
        fprintf(stderr, "Error on line %d, %d: Got incorrectly "
                "matched brackets \"%c\" and \"%c\" while parsing "
                "expression.", stack->closing_token.row,
                stack->closing_token.column, top->op.id,
                stack->closing_token.id);
        exit(EXIT_FAILURE);
    } else if (top->type == PARTIAL_PAREN) {
        /* Resolve the brackets. */
        stack->lhs.count -= 1;
        stack->have_closing_token = false;
        /* Keep have_next_ref, for the subexpression that we just build. */
    } else if (top->type == PARTIAL_INDEX) {
        top->arg_count += 1;
        if (top->arg_count > 1) {
            fprintf(stderr, "Error at line %d, %d: Multidimensional array "
                "index is not yet supported.\n",
                stack->closing_token.row, stack->closing_token.column);
            exit(EXIT_FAILURE);
        }

        /* '[' is listed as the binary operation for array indexing, even
           though that's not how it is parsed. We can still *pretend* that is
           how it was parsed, though! */
        struct pattern_command op = {PATTERN_BINARY};
        op.tk = top->op;
        buffer_push(*out, op);

        /* Resolve the brackets. */
        stack->lhs.count -= 1;
        stack->have_closing_token = false;
        /* Keep have_next_ref, for the subexpression that we just build. */
    } else {
        /* Push one last comma, as if it were written manually. */
        op_stack_resolve_arg(stack, out);
        /* This still exists, op_stack_resolve_arg checked so. */
        top = buffer_top(stack->lhs);

        struct pattern_command *open =
            &out->data[top->open_command_index];
        open->arg_count = top->arg_count;
        open->arg_command_count =
            out->count - top->open_command_index - 1;
        open->has_child_struct = top->has_child_struct;

        if (stack->lhs.count >= 2 && (top->type == PARTIAL_TUPLE || top->type == PARTIAL_RECORD)) {
            /* We just pushed a struct literal onto the intermediate buffer,
               so mark its parent as having a struct literal. */
            stack->lhs.data[stack->lhs.count - 2].has_child_struct = true;
        }

        /* Whatever just happened, it gave us an expression, that might fill
           further holes on the left or the right. */
        stack->have_next_ref = true;

        /* And now those opening and closing tokens are resolved. */
        stack->lhs.count--;
        stack->have_closing_token = false;
    }

    return false;
}

struct pattern parse_expression(
    struct tokenizer *tokenizer,
    bool end_on_eol
) {
    struct pattern result = {0};

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
            struct pattern_command op = {PATTERN_BINARY};
            op.tk = top->op;
            buffer_push(result, op);

            /* E.g. goes from {a + b *; c; +}, to {a +; b * c; +}. */
            /* or {a * ( b +; c; )}, to {a * (; b + c; )}. */
            stack.lhs.count--;
        } else if (stack.have_next_ref && stack.have_closing_token) {
            bool done = resolve_closing_token(&stack, &result);

            if (done) {
                put_token_back(tokenizer, stack.closing_token);

                /* Now finish up. */
                buffer_free(stack.lhs);
                return result;
            }
        } else if (!stack.have_next_ref) {
            read_next_ref(tokenizer, &stack, &result);
        } else if (!stack.have_next_op) {
            if (end_on_eol && stack.grouping_count == 0
                && tokenizer_peek_eol(tokenizer))
            {
                /* Pretend there was a semicolon. */
                stack.have_closing_token = true;
                stack.closing_token.id = ';';
                stack.closing_token.it.data = malloc(1);
                stack.closing_token.it.data[0] = ';';
                stack.closing_token.it.length = 1;
                stack.closing_token.row = tokenizer->row;
                stack.closing_token.column = tokenizer->column;
                stack.opening_id = TOKEN_NULL;
            } else {
                read_next_op(tokenizer, &stack, &result);
            }
        } else {
            /* We have a ref and an operation, and they didn't cause anything
               to pop, so push instead, and try again. */
            struct partial_operation new;
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

/* TODO: Should really name this stuff multivalue_etc, because it isn't always
   about emplacement, even thought emplacement was the first case I had to make
   this stack to handle. Or... maybe it should just be emplacement? */
struct emplace_info {
    enum pattern_command_type type;
    size_t alloc_instruction_index;

    /* The index in the intermediates buffer of the output pointer. */
    size_t pointer_intermediate_index;

    int args_handled;
    int args_total;
    int size; /* Total size for structs, per-element size for arrays. */
    struct type *element_type;

    struct proc_call_info call_info;
};

struct emplace_stack {
    struct emplace_info *data;
    size_t count;
    size_t capacity;
};

void compile_begin_emplace(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates,
    struct emplace_stack *emplace_stack,
    struct pattern_command *c
) {
    struct emplace_info *next_emplace = buffer_addn(*emplace_stack, 1);
    next_emplace->type = c->type;
    if (c->type == PATTERN_ARRAY) {
        next_emplace->alloc_instruction_index = out->count;
        buffer_change_count(*out, 1);
        next_emplace->size = 0;

        struct type array_type = type_array_of(type_int64);
        push_intermediate(intermediates, array_type);

        next_emplace->pointer_intermediate_index = intermediates->count - 1;
    } else if (c->type == PATTERN_STRUCT) {
        next_emplace->alloc_instruction_index = out->count;
        buffer_change_count(*out, 1);
        next_emplace->size = 0;

        push_intermediate(intermediates, type_empty_tuple);
        struct intermediate *val = buffer_top(*intermediates);
        val->owns_stack_memory = true;

        next_emplace->pointer_intermediate_index = intermediates->count - 1;
    } else if (c->type == PATTERN_PROCEDURE_CALL) {
        struct intermediate *proc_val = buffer_top(*intermediates);
        if (proc_val->type.connective != TYPE_PROCEDURE) {
            fprintf(stderr, "Error: Procedure call pattern did not have a procedure to apply to?\n");
            exit(EXIT_FAILURE);
        }
        struct type_buffer outputs = proc_val->type.proc.outputs;
        size_t output_bytes = 0;
        for (int i = 0; i < outputs.count; i++) {
            struct type *it = &outputs.data[i];
            if (it->connective == TYPE_TUPLE || it->connective == TYPE_RECORD) {
                output_bytes += it->total_size;
            }
        }

        bool has_input_memory = c->has_child_struct;

        /* Mark no alloc instruction, because although we might allocate
           something, we already know how big it is, and don't need to come
           back to edit it. */
        next_emplace->alloc_instruction_index = -1;
        next_emplace->size = 0;

        next_emplace->call_info.keep_output_memory = output_bytes > 0;
        if (output_bytes == 0 && has_input_memory) {
            output_bytes = 8;
        }
        if (output_bytes > 0) {
            struct ref temp_memory = {REF_TEMPORARY, intermediates->next_local_index};
            intermediates->next_local_index += 1;

            if (proc_val->ref.type == REF_TEMPORARY && proc_val->ref.x == temp_memory.x - 1) {
                /* Our naive allocation of a temp memory register is in the way
                   of the procedure handle itself, so move the procedure
                   forward, and use the old spot for the memory register
                   instead. */
                compile_mov(out, temp_memory, proc_val);
                proc_val->ref.x += 1;
                temp_memory.x -= 1;
            }

            struct instruction *alloc_instr = buffer_addn(*out, 1);
            alloc_instr->op = OP_STACK_ALLOC;
            alloc_instr->flags = 0;
            alloc_instr->output = temp_memory;
            alloc_instr->arg1.type = REF_CONSTANT;
            alloc_instr->arg1.x = output_bytes;
            alloc_instr->arg2.type = REF_NULL;
            alloc_instr->arg2.x = 0;

            next_emplace->call_info.temp_memory = temp_memory;
        } else {
            next_emplace->call_info.temp_memory.type = REF_NULL;
        }
        next_emplace->call_info.output_bytes = output_bytes;
        next_emplace->call_info.has_input_memory = has_input_memory;

        next_emplace->pointer_intermediate_index = intermediates->count - 1;
    } else {
        fprintf(stderr, "Error at line %d, %d: Got unknown pattern command %d "
            "from token \"", c->tk.row, c->tk.column, c->type);
        fputstr(c->tk.it, stderr);
        fprintf(stderr, "\".\n");
        exit(EXIT_FAILURE);
    }
    next_emplace->args_handled = 0;
    next_emplace->args_total = c->arg_count;
}

void compile_end_arg(
    struct instruction_buffer *out,
    struct intermediate_buffer *intermediates,
    struct emplace_info *em,
    struct pattern_command *c
) {
    if (em->type == PATTERN_ARRAY) {
        struct intermediate val = *buffer_top(*intermediates);
        struct intermediate *pointer_val =
            &intermediates->data[em->pointer_intermediate_index];
        if (em->args_handled == 0) {
            em->size = val.type.total_size;

            /* TODO: actually garbage collect this type info?? idk */
            struct type *ty = malloc(sizeof(struct type));
            *ty = val.type;
            em->element_type = ty;
            pointer_val->type.inner = ty;
        } else {
            /* TODO: properly compare types to make sure the elements of
               the array all agree */
            if (em->size != val.type.total_size) {
                fprintf(stderr, "Error at line %d, %d: Array elements had "
                    "different sizes.\n", c->tk.row,
                    c->tk.column);
                exit(EXIT_FAILURE);
            }
        }
        if (val.type.connective == TYPE_INT || val.type.connective == TYPE_PROCEDURE) {
            /* TODO: Make procedures have enclosed state, and handle that
               appropriately. */
            struct instruction instr;
            instr.op = OP_ARRAY_STORE;
            instr.flags = OP_64BIT;
            instr.output = pointer_val->ref;
            instr.arg1.type = REF_CONSTANT;
            instr.arg1.x = em->args_handled;
            instr.arg2 = val.ref;
            buffer_push(*out, instr);
        } else if (val.type.connective == TYPE_ARRAY) {
            struct instruction instr;
            instr.op = OP_ARRAY_STORE;
            instr.flags = OP_SHARED_BUFF;
            instr.output = pointer_val->ref;
            instr.arg1.type = REF_CONSTANT;
            instr.arg1.x = em->args_handled;
            instr.arg2 = val.ref;
            buffer_push(*out, instr);
        } else {
            /* We immediately use our own temporary, so we don't need to add
               anything to the intermediates buffer. */
            struct ref offset_ptr = push_intermediate(intermediates, val.type);

            struct instruction *instr = buffer_addn(*out, 1);
            instr->op = OP_ARRAY_OFFSET;
            instr->output = offset_ptr;
            instr->arg1 = pointer_val->ref;
            instr->arg2.type = REF_CONSTANT;
            instr->arg2.x = em->args_handled;

            compile_copy(out, intermediates, offset_ptr, &val, false);
            pop_intermediate(intermediates);
        }
        /* Pop after, now that we have finished making and using our own
           temporaries. */
        pop_intermediate(intermediates);
    } else if (em->type == PATTERN_PROCEDURE_CALL) {
        compile_push(out, intermediates);
    } else if (em->type == PATTERN_STRUCT) {
        struct intermediate *pointer_val =
            &intermediates->data[em->pointer_intermediate_index];
        if (c->identifier.id != TOKEN_NULL) {
            /* Emplace arg with an identifier, build a record type. */
            if (pointer_val->type.connective == TYPE_TUPLE) {
                if (pointer_val->type.elements.count == 0) {
                    /* No point freeing the elements list, since clearly
                       nothing has been pushed to it yet. */
                    pointer_val->type = type_empty_record;
                } else {
                    fprintf(stderr, "Error: Got record element in a tuple "
                        "type.\n");
                    exit(EXIT_FAILURE);
                }
            }
            if (pointer_val->type.connective != TYPE_RECORD) {
                fprintf(stderr, "Error: Tried compiling record emplace "
                    "command to an output that wasn't a record?\n");
                exit(EXIT_FAILURE);
            }

            size_t offset = pointer_val->type.total_size;
            struct type val_type = compile_store_top(out, pointer_val->ref, offset, intermediates);

            pointer_val->type.total_size += val_type.total_size;

            struct field *new = buffer_addn(pointer_val->type.fields, 1);
            new->name = c->identifier.it;
            new->type = val_type;
        } else {
            /* No identifier attached to the arg, build a tuple literal. */
            if (pointer_val->type.connective == TYPE_RECORD) {
                fprintf(stderr, "Error: Got bare tuple element in a record type.\n");
                exit(EXIT_FAILURE);
            }
            if (pointer_val->type.connective != TYPE_TUPLE) {
                fprintf(stderr, "Error: Tried compiling tuple emplace command to "
                    "an output that wasn't a tuple?\n");
                exit(EXIT_FAILURE);
            }

            size_t offset = pointer_val->type.total_size;
            struct type val_type = compile_store_top(out, pointer_val->ref, offset, intermediates);

            pointer_val->type.total_size += val_type.total_size;
            buffer_push(pointer_val->type.elements, val_type);
        }
    } else {
        fprintf(stderr, "Error at line %d, %d: Multi-value "
            "encountered with unknown emplace type %d.\n",
            c->tk.row, c->tk.column,
            em->type);
        exit(EXIT_FAILURE);
    }
}

void compile_end_emplace(
    struct instruction_buffer *out,
    int local_count,
    struct intermediate_buffer *intermediates,
    struct emplace_info *em,
    struct pattern_command *c
) {
    if (em->type == PATTERN_PROCEDURE_CALL) {
        em->call_info.arg_count = em->args_total;
        compile_proc_call(
            out,
            local_count,
            intermediates,
            &em->call_info
        );
    } else if (em->type == PATTERN_ARRAY) {
        struct intermediate *pointer_val =
            &intermediates->data[em->pointer_intermediate_index];
        struct instruction *alloc_instr =
            &out->data[em->alloc_instruction_index];
        alloc_instr->op = OP_ARRAY_ALLOC;
        alloc_instr->flags = 0;
        alloc_instr->output = pointer_val->ref;
        alloc_instr->arg1.type = REF_STATIC_POINTER;
        alloc_instr->arg1.x = (int64)em->element_type;
        alloc_instr->arg2.type = REF_CONSTANT;
        alloc_instr->arg2.x = em->args_total;
    } else if (em->type == PATTERN_STRUCT) {
        struct intermediate *pointer_val =
            &intermediates->data[em->pointer_intermediate_index];
        pointer_val->alloc_size = pointer_val->type.total_size;
        struct instruction *alloc_instr =
            &out->data[em->alloc_instruction_index];
        alloc_instr->op = OP_STACK_ALLOC;
        alloc_instr->flags = 0;
        alloc_instr->output = pointer_val->ref;
        alloc_instr->arg1.type = REF_CONSTANT;
        alloc_instr->arg1.x = (int64)pointer_val->type.total_size;
        alloc_instr->arg2.type = REF_NULL;
        alloc_instr->arg2.x = 0;
    } else {
        fprintf(stderr, "Error at line %d, %d: Multi-value "
            "encountered with unknown emplace type %d.\n",
            c->tk.row, c->tk.column,
            em->type);
        exit(EXIT_FAILURE);
    }
}

void compile_expression_inner(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct pattern *in,
    struct intermediate_buffer *intermediates,
    bool is_assignment_lhs
) {
    struct emplace_stack emplace_stack = {0};

    for (int i = 0; i < in->count; i++) {
        struct pattern_command *c = &in->data[i];
        if (c->type == PATTERN_VALUE) {
            compile_value_token(bindings, intermediates, &c->tk);
        } else if (c->type == PATTERN_UNARY) {
            fprintf(stderr, "Error: Unary operators are not yet "
                "implemented.\n");
            exit(EXIT_FAILURE);
        } else if (c->type == PATTERN_BINARY) {
            /* TODO: detect if this is about to be assigned to a variable, and
               use that as the output if so. */
            compile_operation(
                out,
                bindings,
                intermediates,
                c->tk,
                is_assignment_lhs
            );
        } else if (c->type == PATTERN_MEMBER) {
            compile_struct_member(
                out,
                bindings,
                intermediates,
                c->tk,
                is_assignment_lhs
            );
        } else if (c->type == PATTERN_END_TERM) {
            if (emplace_stack.count != 0) {
                fprintf(stderr, "Error: Got multivalue command in the middle "
                    "of a function argument list, or struct/array "
                    "literal...?\n");
                exit(EXIT_FAILURE);
            }
            if (!is_assignment_lhs) {
                /* We are either assigning or returning this multi-value, push it
                   to the stack. */
                compile_push(out, intermediates);
            }
        } else if (c->type == PATTERN_END_ARG) {
            struct emplace_info *em = buffer_top(emplace_stack);
            if (!em) {
                fprintf(stderr, "Error at line %d, %d: Got an END_ARG command "
                    "outside of a function/array/struct expression?\n",
                    c->tk.row, c->tk.column);
                exit(EXIT_FAILURE);
            }
            compile_end_arg(out, intermediates, em, c);
            em->args_handled += 1;
            if (em->args_handled >= em->args_total) {
                int local_count = bindings->count - bindings->global_count;
                compile_end_emplace(out, local_count, intermediates, em, c);
                emplace_stack.count -= 1;
            }
        } else {
            if (is_assignment_lhs) {
                fprintf(stderr, "Error at line %d, %d: Got literal \"", c->tk.row, c->tk.column);
                fputstr(c->tk.it, stderr);
                fprintf(stderr, "\" on left hand side of an assignment.\n");
                exit(EXIT_FAILURE);
            }
            /* Some kind of opening operation, push it to the emplace stack. */
            compile_begin_emplace(out, intermediates, &emplace_stack, c);
        }
    }

    buffer_free(emplace_stack);
}

struct intermediate_buffer compile_expression(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct pattern *in
) {
    struct intermediate_buffer intermediates = intermediates_start(bindings);
    compile_expression_inner(out, bindings, in, &intermediates, false);

    return intermediates;
}

void assert_match_pattern(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct pattern *pattern,
    struct intermediate_buffer *values,
    bool global
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

        struct pattern_command *c = buffer_top(*pattern);
        if (c->type != PATTERN_VALUE) {
            fprintf(stderr, "Error at line %d, %d: The operator \"",
                c->tk.row, c->tk.column);
            fputstr(c->tk.it, stderr);
            fprintf(stderr, "\" appeared on the left hand side of an "
                "assignment statement. Pattern matching is not "
                "implemented.\n");
            exit(EXIT_FAILURE);
        }
        if (c->tk.id != TOKEN_ALPHANUM) {
            fprintf(stderr, "Error at line %d, %d: The literal \"",
                c->tk.row, c->tk.column);
            fputstr(c->tk.it, stderr);
            fprintf(stderr, "\" appeared on the left hand side of an "
                "assignment statement. Pattern matching is not "
                "implemented.\n");
            exit(EXIT_FAILURE);
        }

        size_t global_index = bindings->count;
        struct record_entry *new = buffer_addn(*bindings, 1);
        new->name = c->tk.it;
        struct intermediate val = buffer_pop(*values);
        new->type = val.type;
        new->is_var = c->is_var;

        struct ref new_var;
        if (global) {
            new_var.type = REF_GLOBAL;
            new_var.x = global_index;
            bindings->global_count = bindings->count;
        } else {
            new_var.type = REF_LOCAL;
            new_var.x = global_index - bindings->global_count;
        }

        if (!val.is_pointer) {
            compile_mov(out, new_var, &val);
        } else if (!val.owns_stack_memory) {
            /* Is a pointer, but is not owned. Allocate some new memory and
               copy the value in. */
            /* Is it okay to pass values into this thing as our
               intermediate buffer? */
            compile_copy(out, values, new_var, &val, true);
        } else {
            /* The value is a temporary that was already built on the stack.
               Just take ownership of that memory, in a new binding. */
            if (val.type.total_size < val.alloc_size) {
                /* A struct literal has been constructed and then indexed
                   into. Defragment the now-unused beginning and end of the
                   allocation. */
                realloc_temp_struct(out, values, &val);
            }

            /* The memory is where we need it, now we just need to turn the
               pointer into a local. */
            compile_mov(out, new_var, &val);
        }

        if (pattern->count == 1 && values->count > 0) {
            struct token *tk = &pattern->data[0].tk;
            fprintf(stderr, "Error at line %d, %d: There are more values on "
                "the right hand side of the assignment than on the left hand "
                "side.\n", tk->row, tk->column);
            exit(EXIT_FAILURE);
        }

        pattern->count -= 1;

        /* TODO: Test for multi-value commas and pop them? I don't know. */
    }
}

void compile_assignment(
    struct instruction_buffer *out,
    struct record_table *bindings,
    struct pattern *lhs,
    struct pattern *rhs
) {
    /* Usually the order that we evaluate the LHS vs the RHS does not matter,
       as long as both happen before the actual assignment of the RHS to the
       locations specified on the LHS. In fact, usually the LHS is a no-op,
       just a list of bindings to store to! There are cases where it does
       matter, however; namely when the LHS is an array index, and the RHS
       features that array. e.g.
           var x := [1, 2];
           var y := [3];
           x[0], y = y[0], x;
       When we compile the LHS and RHS of the last line, we clearly want the
       RHS to be (3, [1, 2]), not (3, <magic pointer to x that could change>),
       since this language is about data, not magic pointers to objects that
       can change. This means we need to evaluate the RHS, to get a copy of x,
       before we do the MAKE_UNIQUE operation on the LHS that will prepare x
       for modification. Since in all other cases, it doesn't matter what order
       we evaluate these, we always evaluate the RHS first. */

    struct intermediate_buffer intermediates = compile_expression(out, bindings, rhs);
    size_t rhs_count = intermediates.count;
    size_t pre_lhs_local_count = intermediates.next_local_index;
    /* We want to track how many locals from the RHS are still in use, so that
       when the temporaries on the LHS are gone, we can start popping the RHS
       temporaries too. */
    size_t remaining_rhs_local_count = pre_lhs_local_count;

    compile_expression_inner(out, bindings, lhs, &intermediates, true);
    size_t lhs_count = intermediates.count - rhs_count;

    if (lhs_count != rhs_count) {
        struct token *tk = &lhs->data[0].tk;
        fprintf(stderr, "Error at line %d, %d: There are %d values on the "
            "left hand side of assignment, but %d on the right hand side.\n",
            tk->row, tk->column, lhs_count, rhs_count);
        exit(EXIT_FAILURE);
    }

    for (int i = rhs_count - 1; i >= 0; i--) {
        struct intermediate r = intermediates.data[i];
        struct intermediate l = intermediates.data[rhs_count + i];

        /* TODO: Store a token in each intermediate, representing the first
           token of its subexpression? */
        struct token *err_tk = &lhs->data[0].tk;
        int err_row = err_tk->row;
        int err_col = err_tk->column;
        if (!type_eq(&l.type, &r.type)) {
            fprintf(stderr, "Error at line %d, %d: ", err_row, err_col);
            if (rhs_count == 1) {
                fprintf(stderr, "Assignment had the wrong type.\n");
            } else {
                fprintf(stderr, "Assignment to term %d had the wrong type.\n", i);
            }
            exit(EXIT_FAILURE);
        }

        if (l.ref.type == REF_CONSTANT) {
            fprintf(stderr, "Error at line %d, %d: ", err_row, err_col);
            if (rhs_count == 1) {
                fprintf(stderr, "A literal appeared on the left hand side of an assignment.\n");
            } else {
                fprintf(stderr, "A literal appeared in term %d on the left hand side of an assignment.\n", i);
            }
        }

        bool r_is_l = l.ref.type == r.ref.type && l.ref.x == r.ref.x;

        /* Only do the assignment if the references are different, to avoid
           pushing reference counts to 0 in confusion. */
        if (!r_is_l) {
            compile_variable_decrements(out, l.ref, &l.type, 0, true, false);
            if (l.is_pointer) {
                compile_store(out, l.ref, l.ref_offset, &intermediates, r);
            } else {
                compile_mov(out, l.ref, &r);
            }
        }

        /* Remove one of the LHS intermediates, in case it frees up a variable
           that could be used for pointer offset silliness. */
        pop_intermediate(&intermediates);
        if (r.ref.type == REF_TEMPORARY) {
            remaining_rhs_local_count -= 1;
        }
        /* Also remove from the RHS, if the LHS doesn't have any temporaries
           anymore. */
        /* Do it manually, without popping the intermediates, since otherwise
           we'll lose track of the lhs intermediates. */
        if (intermediates.next_local_index <= pre_lhs_local_count) {
            intermediates.next_local_index = remaining_rhs_local_count;
        }
    }

    /* Popping intermediates doesn't actually do anything if you are going to
       free them anyway. */
    buffer_free(intermediates);
}

#endif
