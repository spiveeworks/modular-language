#ifndef MODLANG_STATEMENTS_H
#define MODLANG_STATEMENTS_H

#include "types.h"

#include "tokenizer.h"
#include "expressions.h"

struct intermediate_buffer parse_statement(
    struct instruction_buffer *out,
    struct tokenizer *tokenizer,
    struct record_table *bindings,
    bool global,
    bool end_on_eol,
    struct type_buffer *return_signature,
    str proc_name,
    bool *all_paths_return_ptr
) {
    if (all_paths_return_ptr) *all_paths_return_ptr = false;

    struct token tk = get_token(tokenizer);
    if (tk.id == TOKEN_RETURN) {
        if (!return_signature) {
            fprintf(stderr, "Error at line %d, %d: Tried to return from the "
                "top level of a file.\n", tk.row, tk.column);
            exit(EXIT_FAILURE);
        }

        struct pattern lhs = parse_expression(tokenizer, end_on_eol);

        tk = get_token(tokenizer);
        if (tk.id != ';') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" after expression.\n");
            exit(EXIT_FAILURE);
        }

        struct intermediate_buffer intermediates = compile_expression(
            out,
            bindings,
            &lhs
        );

        buffer_free(lhs);

        type_check_return(return_signature, &intermediates, proc_name);

        compile_return(out, bindings, &intermediates);

        buffer_free(intermediates);

        if (all_paths_return_ptr) *all_paths_return_ptr = true;
    } else {
        put_token_back(tokenizer, tk);

        struct pattern lhs = parse_expression(tokenizer, end_on_eol);

        tk = get_token(tokenizer);
        if (tk.id == ';') {
            struct intermediate_buffer intermediates = compile_expression(
                out,
                bindings,
                &lhs
            );

            buffer_free(lhs);

            return intermediates;
        } else if (tk.id == TOKEN_DEFINE) {
            struct pattern rhs = parse_expression(tokenizer, false);

            tk = get_token(tokenizer);
            if (tk.id != ';') {
                /* TODO: Make a token assert proc? */
                fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
                fputstr(tk.it, stderr);
                fprintf(stderr, "\"\n");
                exit(EXIT_FAILURE);
            }

            /* TODO: reuse intermediates buffer. */
            struct intermediate_buffer intermediates = compile_expression(
                out,
                bindings,
                &rhs
            );
            buffer_free(rhs);

            assert_match_pattern(out, bindings, &lhs, &intermediates, global);

            buffer_free(lhs);
            buffer_free(intermediates);
        } else if (tk.id == '=') {
            struct pattern rhs = parse_expression(tokenizer, false);

            tk = get_token(tokenizer);
            if (tk.id != ';') {
                /* TODO: Make a token assert proc? */
                fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
                fputstr(tk.it, stderr);
                fprintf(stderr, "\"\n");
                exit(EXIT_FAILURE);
            }

            compile_assignment(out, bindings, &lhs, &rhs);

            buffer_free(lhs);
            buffer_free(rhs);
        } else {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" after expression.\n");
            exit(EXIT_FAILURE);
        }
    }

    /* Returned or assigned results or something, return nothing. */
    return intermediates_start(bindings);
}

/**************/
/* Procedures */
/**************/

struct type parse_type_name(struct token tk) {
    if (!str_eq(tk.it, from_cstr("Int"))) {
        fprintf(stderr, "Error at line %d, %d: Currently only Int, array, "
            "tuple, and record parameters are supported.\n", tk.row, tk.column);
        exit(EXIT_FAILURE);
    }

    return type_int64;
}

struct type parse_type(struct tokenizer *tokenizer) {
    struct token tk = get_token(tokenizer);

    if (tk.id == '[') {
        struct type inner = parse_type(tokenizer);
        struct type result = type_array_of(inner);
        tk = get_token(tokenizer);

        if (tk.id != ']') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter/output type.\n");
            exit(EXIT_FAILURE);
        }

        return result;
    }
    /* else */
    if (tk.id == '{') {
        bool got_element = false;
        struct type result = {0};
        while (true) {
            tk = get_token(tokenizer);
            if (tk.id == '}') break;

            if (tk.id == TOKEN_ALPHANUM) {
                struct token name_tk = tk;

                tk = get_token(tokenizer);
                if (tk.id == ':') {
                    if (got_element && result.connective != TYPE_RECORD) {
                        fprintf(stderr, "Error at line %d, %d: Cannot mix anonymous elements with named fields in a single tuple/record type.", tk.row, tk.column);
                        exit(EXIT_FAILURE);
                    }
                    struct type ty = parse_type(tokenizer);

                    result.connective = TYPE_RECORD;
                    got_element = true;

                    struct field *new = buffer_addn(result.fields, 1);
                    new->name = name_tk.it;
                    new->type = ty;

                    result.total_size += ty.total_size;
                } else {
                    if (got_element && result.connective != TYPE_TUPLE) {
                        fprintf(stderr, "Error at line %d, %d: Cannot mix anonymous elements with named fields in a single tuple/record type.", tk.row, tk.column);
                        exit(EXIT_FAILURE);
                    }
                    struct type ty = parse_type_name(name_tk);

                    result.connective = TYPE_TUPLE;
                    got_element = true;

                    buffer_push(result.elements, ty);

                    result.total_size += ty.total_size;

                    put_token_back(tokenizer, tk);
                }
            } else {
                if (got_element && result.connective != TYPE_TUPLE) {
                    fprintf(stderr, "Error at line %d, %d: Cannot mix anonymous elements with named fields in a single tuple/record type.", tk.row, tk.column);
                    exit(EXIT_FAILURE);
                }

                put_token_back(tokenizer, tk);
                struct type ty = parse_type(tokenizer);

                result.connective = TYPE_TUPLE;
                got_element = true;

                buffer_push(result.elements, ty);
            }

            tk = get_token(tokenizer);
            if (tk.id == '}') break;

            if (tk.id != ',') {
                if (got_element && result.connective == TYPE_RECORD) {
                    fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                        tk.row, tk.column);
                    fputstr(tk.it, stderr);
                    fprintf(stderr, "\" in record type.\n");
                    exit(EXIT_FAILURE);
                } else {
                    fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                        tk.row, tk.column);
                    fputstr(tk.it, stderr);
                    fprintf(stderr, "\" in record type.\n");
                    exit(EXIT_FAILURE);
                }
            }
        }

        return result;
    }

    if (tk.id != TOKEN_ALPHANUM) {
        fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
            tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" in parameter/output type.\n");
        exit(EXIT_FAILURE);
    }

    return parse_type_name(tk);
}

struct record_entry parse_procedure(
    struct instruction_buffer *out,
    struct tokenizer *tokenizer,
    struct record_table *bindings
) {
    struct token tk = get_token(tokenizer);
    if (tk.id != TOKEN_ALPHANUM) {
        fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
            tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" after function/procedure keyword.\n");
        exit(EXIT_FAILURE);
    }

    str proc_name = tk.it;

    int prev_binding_count = bindings->count;

    tk = get_token(tokenizer);
    if (tk.id != '(') {
        fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
            tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" after function/procedure keyword.\n");
        exit(EXIT_FAILURE);
    }

    struct type_buffer input_types = {0};

    while (true) {
        tk = get_token(tokenizer);
        if (tk.id == ')') {
            break;
        }
        /* else */

        bool is_var = false;
        if (tk.id == TOKEN_VAR) {
            is_var = true;
            tk = get_token(tokenizer);
        }

        if (tk.id != TOKEN_ALPHANUM) {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter list.\n");
        }
        str name = tk.it;

        tk = get_token(tokenizer);
        if (tk.id != ':') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter list.\n");
            exit(EXIT_FAILURE);
        }

        struct type ty = parse_type(tokenizer);

        buffer_push(input_types, ty);

        struct record_entry *new = buffer_addn(*bindings, 1);
        new->name = name;
        new->type = ty;
        new->is_var = is_var;

        tk = get_token(tokenizer);
        if (tk.id == ')') {
            break;
        }
        /* else */
        if (tk.id != ',') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter list.\n");
            exit(EXIT_FAILURE);
        }
    }
    bindings->arg_count = bindings->count - bindings->global_count;

    tk = get_token(tokenizer);

    struct type_buffer output_types = {0};
    bool result_specified = false;
    bindings->out_ptr_count = 0;

    if (tk.id == TOKEN_ARROW) {
        struct type ty = parse_type(tokenizer);
        result_specified = true;
        if (ty.connective == TYPE_TUPLE || ty.connective == TYPE_RECORD) {
            bindings->out_ptr_count += 1;
        }
        buffer_push(output_types, ty);

        tk = get_token(tokenizer);
    }

    if (tk.id == TOKEN_DEFINE) {
        struct pattern lhs = parse_expression(tokenizer, false);

        struct token tk = get_token(tokenizer);
        if (tk.id != ';') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in procedure/function body.\n");
            exit(EXIT_FAILURE);
        }

        struct intermediate_buffer intermediates = compile_expression(
            out,
            bindings,
            &lhs
        );

        buffer_free(lhs);

        /* TODO: Unify these outputs */
        if (!result_specified) {
            for (int i = 0; i < intermediates.count; i++) {
                struct type *it = &intermediates.data[i].type;
                if (it->connective == TYPE_TUPLE || it->connective == TYPE_RECORD) {
                    fprintf(stderr, "Error on line %d: Currently one-line "
                        "functions/procedures require signatures if their "
                        "output/s include a tuple or record type.", tk.row);
                    exit(EXIT_FAILURE);
                }
                buffer_push(output_types, *it);
            }
        } else {
            type_check_return(&output_types, &intermediates, proc_name);
        }

        compile_return(out, bindings, &intermediates);

        buffer_free(intermediates);
    } else if (tk.id == '{') {
        bool have_returned = false;
        bool have_warned = false;
        while (true) {
            tk = get_token(tokenizer);
            if (tk.id == '}') break;
            /* else */

            put_token_back(tokenizer, tk);
            if (have_returned && !have_warned) {
                /* Got a statement after we already returned, print a
                   warning. */
                fprintf(stderr, "Warning at line %d, %d: Statement cannot be "
                    "reached.\n", tk.row, tk.column);
                have_warned = true;
            }
            bool statement_returns = false;
            struct intermediate_buffer intermediates = parse_statement(
                out,
                tokenizer,
                bindings,
                false, /* Declarations are not globals. */
                false, /* Do not parse like it is an expression in the REPL. */
                &output_types,
                proc_name,
                &statement_returns
            );
            if (statement_returns) have_returned = true;
            /* If the statement was a bare expression, discard the results of
               that expression. */
            compile_multivalue_decrements(out, &intermediates);
            buffer_free(intermediates);
        }

        if (!have_returned) {
            if (output_types.count == 0) {
                struct intermediate_buffer intermediates = intermediates_start(bindings);
                compile_return(out, bindings, &intermediates);
            } else {
                fprintf(stderr, "Error at line %d, %d: The function \"",
                    tk.row, tk.column);
                fputstr(proc_name, stderr);
                fprintf(stderr, "\" might not return a value.\n");
            }
        }
    } else {
        fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
            tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" in parameter list.\n");
        exit(EXIT_FAILURE);
    }

    bindings->count = prev_binding_count;
    bindings->out_ptr_count = 0;
    bindings->arg_count = 0;

    struct record_entry result;
    result.name = proc_name;
    result.type = type_proc(input_types, output_types);
    result.is_var = false;
    return result;
}

/*******************/
/* Top Level Items */
/*******************/

enum item_type {
    ITEM_NULL,
    ITEM_STATEMENT,
    ITEM_PROCEDURE,
};

struct item {
    enum item_type type;
    struct instruction_buffer instructions;
    struct record_entry proc_binding;
    struct intermediate_buffer intermediates;
};

struct item parse_item(
    struct tokenizer *tokenizer,
    struct record_table *bindings,
    bool repl
) {
    struct item result = {0};

    struct token tk = get_token(tokenizer);
    if (tk.id == TOKEN_EOF) {
        result.type = ITEM_NULL; /* Not technically necessary. */
    } else if (tk.id == TOKEN_FUNC || tk.id == TOKEN_PROC) {
        struct instruction_buffer out = {0};
        result.proc_binding = parse_procedure(&out, tokenizer, bindings);
        result.type = ITEM_PROCEDURE;
        result.instructions = out;
    } else {
        struct instruction_buffer out = {0};
        put_token_back(tokenizer, tk);
        struct intermediate_buffer intermediates = parse_statement(
            &out,
            tokenizer,
            bindings,
            true,
            repl,
            NULL,
            (str){NULL, 0},
            NULL
        );

        result.type = ITEM_STATEMENT;
        result.instructions = out;
        result.intermediates = intermediates;
    }

    return result;
}

#endif
