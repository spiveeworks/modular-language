#ifndef MODLANG_STATEMENTS_H
#define MODLANG_STATEMENTS_H

#include "types.h"

#include "tokenizer.h"
#include "expressions.h"

void parse_assignment(
    struct instruction_buffer *out,
    struct tokenizer *tokenizer,
    struct record_table *bindings
) {
    struct expr_parse_result lhs = parse_expression(tokenizer);

    struct token tk = get_token(tokenizer);
    if (tk.id == TOKEN_DEFINE) {
        if (lhs.has_ref_decl) {
            fprintf(stderr, "Error: \'ref\' is not yet supported.\n");
            exit(EXIT_FAILURE);
        }

        struct expr_parse_result rhs = parse_expression(tokenizer);

        tk = get_token(tokenizer);
        if (tk.id != ';') {
            /* TODO: Make a token assert proc? */
            fprintf(stderr, "Error at line %d, %d: Unexpected token \"",
                tk.row, tk.column);
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

        assert_match_pattern(out, bindings, &lhs.atoms, &intermediates);

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

/*******************/
/* Top Level Items */
/*******************/

enum item_type {
    ITEM_NULL,
    ITEM_STATEMENT,
};

struct item {
    enum item_type type;
    struct instruction_buffer statement_code;
};

struct item parse_item(
    struct tokenizer *tokenizer,
    struct record_table *bindings
) {
    struct item result = {0};;

    struct token tk = get_token(tokenizer);
    if (tk.id == TOKEN_EOF) {
        result.type = ITEM_NULL; /* Not technically necessary. */
    } else {
        struct instruction_buffer out = {0};
        put_token_back(tokenizer, tk);
        parse_assignment(&out, tokenizer, bindings);

        result.type = ITEM_STATEMENT;
        result.statement_code = out;
    }

    return result;
}

#endif
