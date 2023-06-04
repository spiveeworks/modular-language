#ifndef MODLANG_TOKENIZER_H
#define MODLANG_TOKENIZER_H

#include "types.h"

struct char_buffer {
    char *data;
    size_t count;
    size_t capacity;
};

struct tokenizer {
    FILE *input;

    int row;
    int column;

    bool eof;
    struct char_buffer blob;
    size_t blob_chars_read;

    bool has_peek_token;
    struct token peek_token;
};

struct tokenizer start_tokenizer(FILE *input) {
    /* Make sure to start on line 1! */
    return (struct tokenizer){input, 1, 0};
}

void tokenizer_read_input(struct tokenizer *tk) {
    int remaining_count = tk->blob.count - tk->blob_chars_read;
    if (remaining_count < 0) remaining_count = 0;
    if (remaining_count > 0) {
        memmove(
            tk->blob.data,
            tk->blob.data + tk->blob_chars_read,
            remaining_count
        );
    }
    tk->blob.count = remaining_count;
    tk->blob_chars_read = 0;

    buffer_reserve(tk->blob, 128);

    if (fgets(tk->blob.data, tk->blob.capacity - remaining_count, tk->input)) {
        tk->blob.count += strlen(tk->blob.data);
    } else {
        tk->eof = true;
    }
}

char tokenizer_peek_char(struct tokenizer *tk) {
    if (tk->blob_chars_read >= tk->blob.count) {
        /* The buffer is out of characters. If the file isn't already out of
           characters, read some. */
        if (!tk->eof) tokenizer_read_input(tk);
        /* If the file was already out of characters, or if we tried to read
           some characters and it is still out of characters, return null. */
        if (tk->eof) return '\0';
    }

    return tk->blob.data[tk->blob_chars_read];
}

struct token_definition {
    char *cstr;
    enum token_id id;
};

struct token_definition keywords[] = {
    {"func", TOKEN_FUNC},
    {"var", TOKEN_VAR},
    {"ref", TOKEN_REF},
    {"not", TOKEN_LOGIC_NOT},
    {"or", TOKEN_LOGIC_OR},
    {"and", TOKEN_LOGIC_AND},
};

struct token_definition compound_operators[] = {
    {"->", TOKEN_ARROW},
    {":=", TOKEN_DEFINE},

    {"==", TOKEN_EQ},
    {"/=", TOKEN_NEQ},
    {"<=", TOKEN_LEQ},
    {">=", TOKEN_GEQ},
    {"<<", TOKEN_LSHIFT},
    {">>", TOKEN_RSHIFT},
    {"++", TOKEN_CONCAT},
};

bool tokenizer_peek_eol(struct tokenizer *tk) {
    if (tk->has_peek_token) return false;

    while (true) {
        char c = tokenizer_peek_char(tk);
        if (!IS_WHITESPACE(c)) return false;

        if (c == '\r' || c == '\n') return true;

        /* else */
        /* Just any old whitespace. Skip ahead to see whether it was whitespace
           at the end of a line. */
        tk->blob_chars_read += 1;

        tk->column += 1;
    }
}

bool tokenizer_try_read_eol(struct tokenizer *tk) {
    if (tk->has_peek_token) return false;

    while (true) {
        char c = tokenizer_peek_char(tk);
        if (!IS_WHITESPACE(c)) return false;

        if (c == '\r') {
            tk->blob_chars_read += 1;

            tk->row += 1;
            tk->column = 0;

            if (tokenizer_peek_char(tk) == '\n') {
                tk->blob_chars_read += 1;
            }

            return true;
        }
        /* else */
        if (c == '\n') {
            tk->blob_chars_read += 1;

            tk->row += 1;
            tk->column = 0;

            return true;
        }
        /* else */
        /* Just any old whitespace. Skip ahead to see whether it was whitespace
           at the end of a line. */
        tk->blob_chars_read += 1;

        tk->column += 1;
    }
}

void tokenizer_skip_whitespace(struct tokenizer *tk) {
    while (tokenizer_try_read_eol(tk)) {
        /* Do nothing. try_read_eol skips whitespace until it reads a newline,
           all we want is check again. */
    }
}

struct token get_token(struct tokenizer *tk) {
    if (tk->has_peek_token) {
        tk->has_peek_token = false;
        return tk->peek_token;
    }

    /* Whitespace never indicates the start of a token, so skip it. */
    tokenizer_skip_whitespace(tk);

    struct token result;
    result.row = tk->row;
    result.column = tk->column;

    if (tk->blob_chars_read >= tk->blob.count && tk->eof) {
        result.id = TOKEN_EOF;
        result.it.data = NULL;
        result.it.length = 0;
        return result;
    }

    struct char_buffer it = {0};
    char c = tokenizer_peek_char(tk);
    buffer_push(it, c);
    tk->column += 1;
    tk->blob_chars_read += 1;

    if (c < 32) {
        fprintf(stderr, "Error at line %d, %d: Non-printable character "
            "encountered. (Code: %d)\n", tk->row, tk->column, c);
        exit(EXIT_FAILURE);
    } else if (!IS_PRINTABLE(c)) {
        fprintf(stderr, "Error at line %d, %d: Non-ASCII character "
            "encountered.\n", tk->row, tk->column);
        exit(EXIT_FAILURE);
    } else if (IS_ALPHA(c)) {
        while (true) {
            c = tokenizer_peek_char(tk);
            if (!IS_ALPHANUM(c)) break;

            buffer_push(it, c);
            tk->column += 1;
            tk->blob_chars_read += 1;
        }
        result.it.data = it.data;
        result.it.length = it.count;

        result.id = TOKEN_ALPHANUM; /* Default value. */
        for (int i = 0; i < ARRAY_LENGTH(keywords); i++) {
            /* TODO: calculate the keyword lengths up-front somewhere */
            if (str_eq(result.it, from_cstr(keywords[i].cstr))) {
                result.id = keywords[i].id;
                break;
            }
        }
    } else if (IS_NUM(c)) {
        while (true) {
            c = tokenizer_peek_char(tk);
            if (!IS_ALPHANUM(c) && c != '.') break;

            buffer_push(it, c);
            tk->column += 1;
            tk->blob_chars_read += 1;
        }
        result.it.data = it.data;
        result.it.length = it.count;

        result.id = TOKEN_NUMERIC;
    } else {
        result.id = c; /* Default value. */
        tk->blob_chars_read -= 1; /* Undo temporarily */
        bool extended = false;
        for (int i = 0; i < ARRAY_LENGTH(compound_operators); i++) {
            /* TODO: calculate the operator lengths up-front somewhere */
            str op = from_cstr(compound_operators[i].cstr);
            if (op.length > tk->blob.count - tk->blob_chars_read) {
                if (extended) continue;

                tokenizer_read_input(tk);
                extended = true;

                if (op.length > tk->blob.count - tk->blob_chars_read) {
                    continue;
                }
            }

            char *next = tk->blob.data + tk->blob_chars_read;
            if (strncmp(next, op.data, op.length) != 0) continue;

            result.id = compound_operators[i].id;
            buffer_setcount(it, op.length);
            memcpy(it.data, op.data, op.length);
            break;
        }

        result.it.data = it.data;
        result.it.length = it.count;
        tk->column += result.it.length - 1;
        tk->blob_chars_read += result.it.length;
    }

    return result;
}

void put_token_back(struct tokenizer *tokenizer, struct token tk) {
    if (tokenizer->has_peek_token) {
        fprintf(stderr, "Error: Tried to put back multiple tokens?\n");
        exit(EXIT_FAILURE);
    }

    tokenizer->peek_token = tk;
    tokenizer->has_peek_token = true;
}

struct token peek_token(struct tokenizer *tokenizer) {
    if (!tokenizer->has_peek_token) {
        put_token_back(tokenizer, get_token(tokenizer));
    }
    return tokenizer->peek_token;
}

#endif
