#ifndef MODLANG_TYPES_H
#define MODLANG_TYPES_H

/* Not just a specification of a modlang type, but all of the datatypes that
   the language uses. */

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

/* This function might make less sense as integer literal logic is expanded. */
int64 integer_from_string(str it) {
    int64 result = 0;
    for (int i = 0; i < it.length; i++) {
        char c = it.data[i];
        /* This condition is later used to define the IS_NUM macro. */
        if (c > '0' && c <= '9') {
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
/* Tokens */
/**********/

#define IS_LOWER(c) ('a' <= (c) && (c) <= 'z')
#define IS_UPPER(c) ('A' <= (c) && (c) <= 'Z')
#define IS_ALPHA(c) (IS_LOWER(c) || IS_UPPER(c))
#define IS_NUM(c) ('0' <= (c) && (c) <= '9')
#define IS_ALPHANUM(c) (IS_ALPHA(c) || IS_NUM(c) || (c) == '_')
#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')
#define IS_PRINTABLE(c) (' ' <= (c) && (c) <= '~')

enum token_id {
    TOKEN_NULL = 0,

    /* Printable characters are all tokens. */

    TOKEN_ALPHANUM = 128,
    TOKEN_NUMERIC,

    TOKEN_ARROW,
    TOKEN_DEFINE,

    TOKEN_EQ,
    TOKEN_NEQ,
    TOKEN_LEQ,
    TOKEN_GEQ,
    TOKEN_LSHIFT,
    TOKEN_RSHIFT,

    TOKEN_FUNC,
    TOKEN_VAR,
    TOKEN_REF,
    TOKEN_LOGIC_NOT,
    TOKEN_LOGIC_OR,
    TOKEN_LOGIC_AND,
    TOKEN_EOF
};

struct token {
    enum token_id id;
    str it;
    int row;
    int column;
};

/*********/
/* Types */
/*********/

enum type_connective {
    TYPE_INT,
    TYPE_UINT,
    TYPE_WORD,
    TYPE_FLOAT,
    TYPE_TUPLE,
    TYPE_RECORD,
    TYPE_ARRAY,
};

struct record_entry;

struct record_table {
    struct record_entry *data;
    size_t count;
    size_t capacity;

    size_t global_count;
};

struct type {
    enum type_connective connective;
    union {
        uint8 word_size; /* 0 => 8 bits, up to 3 => 64 bits */
        struct record_table fields;
        struct type *inner;
    };
    int32 total_size;
};

struct record_entry {
    str name;
    struct type type;
};

const struct type type_int64 = {
    .connective = TYPE_INT,
    .word_size = 3,
    .total_size = 8
};

/* TODO: work out a memory arena or reference counting or something to make
   these types safe to copy. */
struct type type_array_of(struct type entry_type) {
    struct type result;
    result.connective = TYPE_ARRAY;
    result.inner = malloc(sizeof (struct type));
    *result.inner = entry_type;

    return result;
}

void destroy_type(struct type *it) {
    if (it->connective == TYPE_TUPLE || it->connective == TYPE_RECORD) {
        for (int i = 0; i < it->fields.count; i++) {
            destroy_type(&it->fields.data[i].type);
        }
        buffer_free(it->fields);
    } else if (it->connective == TYPE_ARRAY) {
        destroy_type(it->inner);
        free(it->inner);
    }
};

int lookup_name(struct record_table *table, str name) {
    for (int i = table->count - 1; i >= 0; i--) {
        if (str_eq(name, table->data[i].name)) return i;
    }
    return -1;
}

/****************/
/* Instructions */
/****************/

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
    OP_MOD,
    OP_EDIV,
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

#endif
