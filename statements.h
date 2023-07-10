#ifndef MODLANG_STATEMENTS_H
#define MODLANG_STATEMENTS_H

#include "types.h"

#include "tokenizer.h"
#include "expressions.h"

void parse_statement(
    struct instruction_buffer *out,
    struct tokenizer *tokenizer,
    struct record_table *bindings,
    bool global,
    bool end_on_eol
) {
    struct token tk = get_token(tokenizer);
    if (tk.id == TOKEN_RETURN) {
        struct type_buffer intermediates = {0};
        struct expr_parse_result lhs = parse_expression(tokenizer, end_on_eol);

        tk = get_token(tokenizer);
        if (tk.id != ';') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" after expression.\n");
            exit(EXIT_FAILURE);
        }

        compile_expression(
            out,
            bindings,
            &intermediates,
            &lhs.atoms
        );

        buffer_free(lhs.atoms);

        compile_return(out, &intermediates);
        buffer_free(intermediates);
    } else {
        put_token_back(tokenizer, tk);

        struct expr_parse_result lhs = parse_expression(tokenizer, end_on_eol);

        tk = get_token(tokenizer);
        if (tk.id == ';') {
            struct type_buffer intermediates = {0};

            compile_expression(
                out,
                bindings,
                &intermediates,
                &lhs.atoms
            );

            buffer_free(lhs.atoms);
            buffer_free(intermediates);
        } else if (tk.id == TOKEN_DEFINE) {
            if (lhs.has_ref_decl) {
                fprintf(stderr, "Error: \'ref\' is not yet supported.\n");
                exit(EXIT_FAILURE);
            }

            struct expr_parse_result rhs = parse_expression(tokenizer, false);

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
            struct type_buffer intermediates = {0};
            compile_expression(
                out,
                bindings,
                &intermediates,
                &rhs.atoms
            );
            buffer_free(rhs.atoms);

            assert_match_pattern(out, bindings, &lhs.atoms, &intermediates, global);

            buffer_free(lhs.atoms);
            buffer_free(intermediates);
        } else if (tk.id == '=') {
            fprintf(stderr, "Error: Ref assignment not yet implemented.\n");
            exit(EXIT_FAILURE);
        } else {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                    tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" after expression.\n");
            exit(EXIT_FAILURE);
        }
    }
}

/**************/
/* Procedures */
/**************/

str parse_procedure(
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

    while (true) {
        tk = get_token(tokenizer);
        if (tk.id == ')') {
            break;
        }
        /* else */

        str name = tk.it;

        tk = get_token(tokenizer);
        if (tk.id != ':') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter list.\n");
            exit(EXIT_FAILURE);
        }

        tk = get_token(tokenizer);
        if (tk.id != TOKEN_ALPHANUM) {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in parameter list.\n");
            exit(EXIT_FAILURE);
        }

        if (!str_eq(tk.it, from_cstr("Int"))) {
            fprintf(stderr, "Error at line %d, %d: Currently only Int parameters are "
                "supported.\n", tk.row, tk.column);
            exit(EXIT_FAILURE);
        }

        struct record_entry *new = buffer_addn(*bindings, 1);
        new->name = name;
        new->type = type_int64;

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

    tk = get_token(tokenizer);
    if (tk.id == TOKEN_DEFINE) {
        struct expr_parse_result lhs = parse_expression(tokenizer, false);

        struct token tk = get_token(tokenizer);
        if (tk.id != ';') {
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
            fputstr(tk.it, stderr);
            fprintf(stderr, "\" in procedure/function body.\n");
            exit(EXIT_FAILURE);
        }

        struct type_buffer intermediates = {0};

        compile_expression(
            out,
            bindings,
            &intermediates,
            &lhs.atoms
        );

        buffer_free(lhs.atoms);

        compile_return(out, &intermediates);
        buffer_free(intermediates);
    } else if (tk.id == '{') {
        while (true) {
            struct token tk = get_token(tokenizer);
            if (tk.id == '}') break;
            /* else */

            put_token_back(tokenizer, tk);
            parse_statement(out, tokenizer, bindings, false, false);
        }
    } else {
        fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
            tk.row, tk.column);
        fputstr(tk.it, stderr);
        fprintf(stderr, "\" in parameter list.\n");
        exit(EXIT_FAILURE);
    }

    bindings->count = prev_binding_count;

    return proc_name;
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
    struct instruction_buffer statement_code;
    str name;
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
        result.name = parse_procedure(&out, tokenizer, bindings);
        result.type = ITEM_PROCEDURE;
        result.statement_code = out;
    } else {
        struct instruction_buffer out = {0};
        put_token_back(tokenizer, tk);
        parse_statement(&out, tokenizer, bindings, true, repl);

        result.type = ITEM_STATEMENT;
        result.statement_code = out;
    }

    return result;
}

#endif
