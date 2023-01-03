#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define ARRAY_LENGTH(X) (sizeof(X) / sizeof((X)[0]))

/***********/
/* Strings */
/***********/

typedef struct str {
    char *data;
    long length;
} str;

bool str_eq(str a, str b) {
    if (a.length != b.length) return false;
    return strncmp(a.data, b.data, a.length) == 0;
}

str from_cstr(char *data) {
    return (struct str){data, strlen(data)};
}

void fputstr(str string, FILE *f) {
    fwrite(string.data, 1, string.length, f);
}

/*************/
/* Tokenizer */
/*************/

#define IS_LOWER(c) ('a' <= (c) && (c) <= 'z')
#define IS_UPPER(c) ('A' <= (c) && (c) <= 'Z')
#define IS_ALPHA(c) (IS_LOWER(c) || IS_UPPER(c))
#define IS_NUM(c) ('0' <= (c) && (c) <= '9')
#define IS_ALPHANUM(c) (IS_ALPHA(c) || IS_NUM(c) || (c) == '_')
#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')
#define IS_PRINTABLE(c) (' ' <= (c) && (c) <= '~')

struct tokenizer {
    char *next;
    char *end;
    int row;
    int column;
};

struct tokenizer start_tokenizer(str input) {
    /* Make sure to start on line 1! */
    return (struct tokenizer){input.data, input.data + input.length, 1, 0};
}

enum token_id {
    TOKEN_NULL = 0,

    /* Printable characters are all tokens. */

    TOKEN_ALPHANUM = 128,
    TOKEN_ARROW,
    TOKEN_FUNC,
    TOKEN_EOF
};

struct token_definition {
    char *cstr;
    enum token_id id;
};

struct token_definition keywords[] = {
    {"func", TOKEN_FUNC},
};

struct token_definition compound_operators[] = {
    {"->", TOKEN_ARROW},
};

struct token {
    enum token_id id;
    str it;
    int row;
    int column;
};

struct token get_token(struct tokenizer *tk) {
    while (tk->next < tk->end && IS_WHITESPACE(*tk->next)) {
        if (tk->next + 1 < tk->end && tk->next[0] == '\r' && tk->next[1] == '\n') {
            tk->next += 2;
            tk->row += 1;
            tk->column = 0;
        } else if (*tk->next == '\r' || *tk->next == '\n') {
            tk->next += 1;
            tk->row += 1;
            tk->column = 0;
        } else {
            tk->next += 1;
            tk->column += 1;
        }
    }

    struct token result;
    result.row = tk->row;
    result.column = tk->column;
    result.it.data = tk->next;
    result.it.length = 0;

    if (tk->next >= tk->end) {
        result.id = TOKEN_EOF;
        result.it.length = 0;
    } else if (*tk->next < 32) {
        fprintf(stderr, "Error at line %d, %d: Non-printable character "
            "encountered. (Code: %d)\n", tk->row, tk->column, *tk->next);
        exit(EXIT_FAILURE);
    } else if (!IS_PRINTABLE(*tk->next)) {
        fprintf(stderr, "Error at line %d, %d: Non-ASCII character "
            "encountered.\n", tk->row, tk->column);
        exit(EXIT_FAILURE);
    } else if (IS_ALPHA(*tk->next)) {
        int length = 0;
        while (tk->next < tk->end && IS_ALPHANUM(*tk->next)) {
            tk->next += 1;
            tk->column += 1;
            length += 1;
        }
        result.it.length = length;

        result.id = TOKEN_ALPHANUM; /* Default value. */
        for (int i = 0; i < ARRAY_LENGTH(keywords); i++) {
            if (strncmp(keywords[i], result.it.data, result.it.length) == 0) {
                result.id = keywords[i].id;
                break;
            }
        }
    } else {
        result.id = *tk->next; /* Default value. */
        result.it.length = 1;
        for (int i = 0; i < ARRAY_LENGTH(compound_operators); i++) {
            /* TODO: cache these lengths */
            str op = from_cstr(compound_operators[i].cstr);
            if (tk->next + op.length > tk->end) continue;
            if (strncmp(tk->next, op.data, op.length) != 0) continue;

            result.id = compound_operators[i].id;
            result.it.length = op.length;
            break;
        }

        tk->next += result.it.length;
        tk->column += result.it.length;
    }

    return result;
}

/******/
/* IO */
/******/

str read_file(char *path) {
    FILE *input = NULL;
    str contents;

    /* Open in binary mode, since we already tokenize the \r\n ourselves. */
    input = fopen(path, "rb");
    if (!input) {
        perror("error opening file");
    }

    fseek(input, 0L, SEEK_END);
    contents.length = ftell(input);
    contents.data = malloc(contents.length + 1);

    rewind(input);
    contents.length = fread(contents.data, 1, contents.length, input);
    contents.data[contents.length] = '\0';

    fclose(input);

    return contents;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        fprintf(stderr, "Error: Expected input file.\n");
        exit(EXIT_FAILURE);
    }
    if (argc > 2) {
        fprintf(stderr, "Error: Too many arguments.\n");
        exit(EXIT_FAILURE);
    }

    str input = read_file(argv[1]);

    {
        struct tokenizer tokenizer = start_tokenizer(input);

        while (true) {
            struct token tk = get_token(&tokenizer);
            if (tk.id == TOKEN_EOF) break;

            if (tk.it.length == 1) {
                printf("Line %d, row %d, id %d, \'%c\'\n",
                    tk.row, tk.column, tk.id, tk.it.data[0]);
            } else {
                printf("Line %d, row %d, id %d, \"",
                    tk.row, tk.column, tk.id);
                fwrite(tk.it.data, 1, tk.it.length, stdout);
                printf("\"\n");
            }
        }
    }

    return 0;
}

