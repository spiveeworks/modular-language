#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

typedef int64_t int64;
typedef int32_t int32;
typedef int16_t int16;
typedef int8_t int8;

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;

typedef unsigned int uint;

typedef uint8_t byte;

#define ARRAY_LENGTH(X) (sizeof(X) / sizeof((X)[0]))

/***********/
/* Strings */
/***********/

typedef struct str {
    char *data;
    size_t length;
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
    TOKEN_NUMERIC,
    TOKEN_ARROW,
    TOKEN_EQ,
    TOKEN_NEQ,
    TOKEN_LEQ,
    TOKEN_GEQ,
    TOKEN_LSHIFT,
    TOKEN_RSHIFT,

    TOKEN_FUNC,
    TOKEN_LOGIC_NOT,
    TOKEN_LOGIC_OR,
    TOKEN_LOGIC_AND,
    TOKEN_EOF
};

struct token_definition {
    char *cstr;
    enum token_id id;
};

struct token_definition keywords[] = {
    {"func", TOKEN_FUNC},
    {"not", TOKEN_LOGIC_NOT},
    {"or", TOKEN_LOGIC_OR},
    {"and", TOKEN_LOGIC_AND},
};

struct token_definition compound_operators[] = {
    {"->", TOKEN_ARROW},

    {"==", TOKEN_EQ},
    {"/=", TOKEN_NEQ},
    {"<=", TOKEN_LEQ},
    {">=", TOKEN_GEQ},
    {"<<", TOKEN_LSHIFT},
    {">>", TOKEN_RSHIFT},
};

struct token {
    enum token_id id;
    str it;
    int row;
    int column;
};

struct token get_token(struct tokenizer *tk) {
    while (tk->next < tk->end && IS_WHITESPACE(*tk->next)) {
        if (tk->next + 1 < tk->end
            && tk->next[0] == '\r' && tk->next[1] == '\n')
        {
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
    } else if (IS_NUM(*tk->next)) {
        int length = 0;
        while (tk->next < tk->end
            && (IS_ALPHANUM(*tk->next) || *tk->next == '.'))
        {
            tk->next += 1;
            tk->column += 1;
            length += 1;
        }
        result.it.length = length;

        result.id = TOKEN_NUMERIC;
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

int64 convert_integer_literal(str it) {
    int64 result = 0;
    for (int i = 0; i < it.length; i++) {
        char c = it.data[i];
        if (IS_NUM(c)) {
            result *= 10;
            result += c - '0';
        } else {
            fprintf(stderr, "Error: Got integer literal with unsupported "
                "character '%c' in it.\n", c);
            exit(EXIT_FAILURE);
        }
    }

    return result;
}

/**********/
/* Parser */
/**********/

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

enum ref_type {
    REF_CONSTANT,
    REF_GLOBAL,
    REF_LOCAL,
    REF_TEMPORARY,
};

struct ref {
    enum ref_type type;
    int64 x;
};

struct partial_operation {
    struct ref arg;
    enum token_id op;
    enum precedence_level precedence;
    enum token_id close_token;
};

struct op_stack {
    bool running;
    int64 temp_var_count;

    /* Represents something like  a || b && c == d +  */
    int64 lhs_count;
    int64 lhs_cap;
    struct partial_operation *lhs;

    /* Represents the subsequent e, which either groups to the left or to the
       right. */
    bool have_next_ref;
    struct ref next_ref;

    /* Represents the subsequent operation, which either causes a cascade of
       binary operations to group together, or adds a new partial operation to
       the stack. */
    bool have_next_op;
    enum token_id next_op;
    enum precedence_level next_precedence;
};

struct expr_operation {
    enum token_id operator;
    struct ref output;
    struct ref arg1;
    struct ref arg2;
};

struct op_stack start_parsing_expression(void) {
    struct op_stack result = {0};

    result.running = true;

    return result;
}

struct expr_operation parse_next_operation(
    struct op_stack *stack,
    struct tokenizer *tokenizer
) {
    while (true) {
        if (!stack->have_next_ref) {
            struct token tk = get_token(tokenizer);

            if (tk.id == TOKEN_NUMERIC) {
                stack->next_ref.type = REF_CONSTANT;
                stack->next_ref.x = convert_integer_literal(tk.it);
                stack->have_next_ref = true;
            } else {
                fprintf(stderr, "Error on line %d, %d: Got unexpected token \"",
                    tk.row, tk.column);
                fputstr(tk.it, stderr);
                fprintf(stderr, "\" while parsing expression.\n");
                exit(EXIT_FAILURE);
            }
        } else if (!stack->have_next_op) {
            struct token tk = get_token(tokenizer);
            enum token_id op = tk.id;

            for (int i = 0; i < ARRAY_LENGTH(precedence_info); i++) {
                if (tk.id == precedence_info[i].operator) {
                    stack->next_op = op;
                    stack->next_precedence = precedence_info[i].precedence;
                    stack->have_next_op = true;
                    break;
                }
            }
            if (!stack->have_next_op) {
                fprintf(stderr, "Error on line %d, %d: Got unexpected token \"",
                    tk.row, tk.column);
                fputstr(tk.it, stderr);
                fprintf(stderr, "\" while parsing expression.\n");
                exit(EXIT_FAILURE);
            }
        } else {
            struct partial_operation *top;
            if (stack->lhs_count > 0) {
                top = &stack->lhs[stack->lhs_count - 1];
            } else {
                top = NULL;
            }
            /* e.g. ADDITIVE and MULTIPLICATIVE will both pop a MULTIPLICATIVE,
               but neither will pop a COMPARATIVE.

                   a * b *        a * b +        a < b *        a < b +

               So pop if the new thing is lower (closer to ||) than the old. */
            bool pop = top && top->precedence != PRECEDENCE_GROUPING
                && stack->next_precedence <= top->precedence;
            if (pop) {
                struct expr_operation result;
                /* TODO: make these fields match better? */
                result.arg1 = top->arg;
                result.operator = top->op;
                result.arg2 = stack->next_ref;

                result.output.type = REF_TEMPORARY;
                result.output.x = -1; /* TODO: what should this be?? */

                /* E.g. goes from {a + b *; c; +}, to {a +; b * c; +}. */
                stack->next_ref = result.output;
                stack->lhs_count -= 1;

                return result;
            } else {
                if (stack->lhs_count >= stack->lhs_cap) {
                    if (stack->lhs_cap == 0) {
                        stack->lhs =
                            malloc(16 * sizeof(struct partial_operation));
                        stack->lhs_cap = 16;
                    } else {
                        stack->lhs = realloc(stack->lhs, (stack->lhs_count + 16)
                                * sizeof(struct partial_operation));
                        stack->lhs_cap += 16;
                    }
                }
                /* top may be pointing to memory that has been reallocated, but
                   we reuse it now, to point to the new location we are writing
                   to. */
                top = &stack->lhs[stack->lhs_count];
                top->arg = stack->next_ref;
                top->op = stack->next_op;
                top->precedence = stack->next_precedence;

                stack->lhs_count += 1;

                stack->have_next_ref = false;
                stack->have_next_op = false;

                /* Continue through the loop again. */
            }
        }
    }
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

void print_ref(struct ref ref) {
    switch (ref.type) {
      case REF_CONSTANT:
        printf(" ");
        break;
      case REF_GLOBAL:
        printf("g");
        break;
      case REF_LOCAL:
        printf("l");
        break;
      case REF_TEMPORARY:
        printf("v");
        break;
    }
    printf("%lld", ref.x);
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

        struct op_stack stack = start_parsing_expression();
        while (true) {
            struct expr_operation op = parse_next_operation(&stack, &tokenizer);
            print_ref(op.output);
            printf(" = ");
            print_ref(op.arg1);
            printf(" %c ", op.operator);
            print_ref(op.arg2);
            printf("\n");
        }

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

