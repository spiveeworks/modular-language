#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"

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
            /* TODO: calculate the keyword lengths up-front somewhere */
            if (str_eq(result.it, from_cstr(keywords[i].cstr))) {
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
            /* TODO: calculate the operator lengths up-front somewhere */
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

/*********/
/* Types */
/*********/

enum type_connective {
    TYPE_INT,
    TYPE_UINT,
    TYPE_WORD,
    TYPE_FLOAT,
    TYPE_TUPLE,
    TYPE_ARRAY,
};

struct record_entry;

struct record_table {
    struct record_entry *data;
    size_t count;
    size_t capacity;
};

struct type {
    enum type_connective connective;
    union {
        uint8 size; /* 0 => 8 bits, up to 3 => 64 bits */
        struct record_table fields;
    };
};

struct record_entry {
    str name;
    struct type type;
};

void destroy_type(struct type *it) {
    if (it->connective == TYPE_TUPLE) {
        for (int i = 0; i < it->fields.count; i++) {
            destroy_type(&it->fields.data[i].type);
        }
        buffer_free(it->fields);
    }
};

int lookup_name(struct record_table *table, str name) {
    for (int i = table->count - 1; i >= 0; i--) {
        if (str_eq(name, table->data[i].name)) return i;
    }
    return -1;
}

/*********************/
/* Expression Parser */
/*********************/

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

/* Now we just need to parse those RPN buffers. */
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

struct rpn_ref {
    /* Could use TOKEN_NULL instead of this flag. */
    bool push;
    struct token tk;
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
    struct token next_token;
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

                buffer_free(stack.lhs);

                struct expr_parse_result result;
                result.has_ref_decl = has_ref_decl;
                result.atoms = out;
                result.next_token = stack.closing_token;
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

/*******************/
/* Type Resolution */
/*******************/

enum operation {
    OP_NULL,
    OP_MOV,
    OP_LOR,
    OP_LAND,
    OP_EQ,
    OP_NEQ,
    OP_LEQ,
    OP_GEQ,
    OP_LESS,
    OP_GREATER,
    OP_BOR,
    OP_BAND,
    OP_BXOR,
    OP_PLUS,
    OP_MINUS,
    OP_LSHIFT,
    OP_RSHIFT,
    OP_MUL,
    OP_DIV,
    OP_EDIV,
    OP_MOD,
    OP_EMOD,
};

enum operation_flags {
    OP_8BIT  = 0x0,
    OP_16BIT = 0x1,
    OP_32BIT = 0x2,
    OP_64BIT = 0x3,

    OP_FLOAT = 0x4, /* This is a mask, not a valid value on its own. */
    OP_FLOAT32 = 0x6,
    OP_FLOAT64 = 0x7,
};

enum ref_type {
    REF_NULL,
    REF_CONSTANT,
    REF_GLOBAL,
    REF_LOCAL,
    REF_TEMPORARY,
};

struct ref {
    enum ref_type type;
    int64 x;
};

struct instruction {
    enum operation op;
    enum operation_flags flags;
    struct ref output;
    struct ref arg1;
    struct ref arg2;
};

struct instruction_buffer {
    struct instruction *data;
    size_t count;
    size_t capacity;
};

struct type_buffer {
    struct type *data;
    size_t count;
    size_t capacity;
};

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
        result.connective = TYPE_INT;
        result.size = 3;
        break;
      case REF_GLOBAL:
        fprintf(stderr, "Error: Globals not implemented?\n");
        exit(EXIT_FAILURE);
        break;
      case REF_LOCAL:
        result = bindings->data[it.x].type;
        break;
      case REF_TEMPORARY:
        result = intermediates->data[it.x];
        break;
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

        return (struct ref){REF_LOCAL, ind};
    }
    /* else */
    if (in->id == TOKEN_NUMERIC) {
        int64 value = convert_integer_literal(in->it);
        return (struct ref){REF_CONSTANT, value};
    }
    /* else */

    fprintf(stderr, "Error: Asked to compile \"");
    fputstr(in->it, stderr);
    fprintf(stderr, "\" as an RPN atom?\n");
    exit(EXIT_FAILURE);
}

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
    /* Make a combined rpn_ref -> {ref, type} compile function? */
    struct type arg1_type =
        get_type_info(bindings, intermediates, result.arg1);
    struct type arg2_type =
        get_type_info(bindings, intermediates, result.arg2);

    /* Casing all the scalar connectives sounds annoying. Maybe I should make
       the connective "scalar", or work out some bit mask trick to test them
       all in one go. */
    if (arg1_type.connective != TYPE_INT || arg2_type.connective != TYPE_INT) {
        fprintf(stderr, "Error: Argument to operator %c must be an integer.\n",
            operation.id);
        exit(EXIT_FAILURE);
    }
    if (arg1_type.size != 3 || arg2_type.size != 3) {
        fprintf(stderr, "Error: Currently only 64 bit integer types are "
            "implemented.\n");
        exit(EXIT_FAILURE);
    }
    result.flags = OP_64BIT;

    result.output.type = REF_TEMPORARY;
    result.output.x = intermediate_count;

    buffer_push(*out, result);

    while (intermediates->count > intermediate_count) {
        destroy_type(buffer_top(*intermediates));
        intermediates->count -= 1;
    }

    struct type output_type;
    output_type.connective = TYPE_INT;
    output_type.size = 3;
    buffer_push(*intermediates, output_type);
}

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
            fprintf(stderr, "Error: Mov not yet implemented.\n");
            exit(EXIT_FAILURE);
        } else {
            fprintf(stderr, "Error: Got unknown atom type in RPN "
                "compilation?\n");
            exit(EXIT_FAILURE);
        }

        i = i + j + 1;
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

    struct tokenizer tokenizer = start_tokenizer(input);
    struct record_table bindings = {0};
    struct instruction_buffer program = {0};

    struct expr_parse_result expr = parse_expression(&tokenizer);
    printf("RPN output: ");
    for (int i = 0; i < expr.atoms.count; i++) {
        if (i > 0) printf(" ");
        fputstr(expr.atoms.data[i].tk.it, stdout);
        if (expr.atoms.data[i].type == RPN_BINARY_REVERSE) printf("r");
    }
    printf("\n\n");

    struct type_buffer intermediates = {0};
    compile_expression(
        &program,
        &bindings,
        &intermediates,
        &expr.atoms
    );

    for (int i = 0; i < program.count; i++) {
        struct instruction *instr = &program.data[i];
        print_ref(instr->output);
        printf(" = Op%d ", instr->op);
        print_ref(instr->arg1);
        printf(", ");
        print_ref(instr->arg2);
        printf("\n");
    }

    printf("Produced %llu values.\n", intermediates.count);
    if (expr.next_token.id != TOKEN_EOF) {
        fprintf(stderr, "Error at line %d, %d: Expected a complete"
            " expression, followed by the end of the file.\n",
            expr.next_token.row,
            expr.next_token.column);
        exit(EXIT_FAILURE);
    }

    return 0;
}

