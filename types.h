#ifndef MODLANG_TYPES_H
#define MODLANG_TYPES_H

/* Not just a specification of a modlang type, but all of the datatypes that
   the language uses. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
        if ('0' <= c && c <= '9') {
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
    TOKEN_CONCAT,

    TOKEN_FUNC,
    TOKEN_PROC,
    TOKEN_RETURN,
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
    TYPE_PROCEDURE,
};

struct record_entry;

struct record_table {
    struct record_entry *data;
    size_t count;
    size_t capacity;

    size_t global_count;
    size_t arg_count;
    /* local count (including args) = count - global_count */
};

/* TODO: What should these two structs actually be called? */
struct field_record_table {
    struct record_entry *data;
    size_t count;
    size_t capacity;
};

struct type;

struct type_buffer {
    struct type *data;
    size_t count;
    size_t capacity;
};

struct proc_signature {
    struct type_buffer inputs;
    struct type_buffer outputs;
};

struct type {
    enum type_connective connective;
    union {
        uint8 word_size; /* 0 => 8 bits, up to 3 => 64 bits */
        struct type_buffer elements;
        struct field_record_table fields;
        struct type *inner;
        struct proc_signature proc;
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

const struct type type_empty_tuple = {
    .connective = TYPE_TUPLE,
    .total_size = 0
};

/* Not sure whether this distinct type will exist from the user's perspective,
   but this is still a useful starting point for building record types. */
const struct type type_empty_record = {
    .connective = TYPE_RECORD,
    .total_size = 0
};

/* TODO: work out a memory arena or reference counting or something to make
   these types safe to copy. */
struct type type_array_of(struct type entry_type) {
    struct type result;
    result.connective = TYPE_ARRAY;
    result.inner = malloc(sizeof (struct type));
    *result.inner = entry_type;
    /* TODO: reorganise shared_buffer to be in a place that lets us sizeof it
       from here. */
    result.total_size = 16;

    return result;
}

struct type type_proc(struct type_buffer inputs, struct type_buffer outputs) {
    struct type result;
    result.connective = TYPE_PROCEDURE;
    result.proc.inputs = inputs;
    result.proc.outputs = outputs;
    /* TODO: Give all functions enclosed data? */
    result.total_size = 8;

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

int lookup_name_fields(struct field_record_table *table, str name) {
    for (int i = table->count - 1; i >= 0; i--) {
        if (str_eq(name, table->data[i].name)) return i;
    }
    return -1;
}

/*****************/
/* Type Checking */
/*****************/

bool type_eq(struct type *a, struct type *b) {
    if (a->connective != b->connective) {
        return false;
    }

    switch (a->connective) {
    case TYPE_INT:
        return a->word_size == b->word_size;
    case TYPE_UINT:
        return a->word_size == b->word_size;
    case TYPE_WORD:
        return a->word_size == b->word_size;
    case TYPE_FLOAT:
        return a->word_size == b->word_size;
    case TYPE_TUPLE:
        if (a->elements.count != b->elements.count) return false;
        for (int i = 0; i < a->elements.count; i++) {
            if (!type_eq(&a->elements.data[i], &b->elements.data[i])) {
                return false;
            }
        }
        return true;
    case TYPE_RECORD:
        if (a->fields.count != b->fields.count) return false;
        for (int i = 0; i < a->fields.count; i++) {
            struct record_entry *a_entry = &a->fields.data[i];
            struct record_entry *b_entry = &b->fields.data[i];
            if (!str_eq(a_entry->name, b_entry->name)) {
                return false;
            }
            if (!type_eq(&a_entry->type, &b_entry->type)) {
                return false;
            }
        }
        return true;
    case TYPE_ARRAY:
        return type_eq(a->inner, b->inner);
    case TYPE_PROCEDURE:
        if (a->proc.inputs.count != b->proc.inputs.count) return false;
        if (a->proc.outputs.count != b->proc.outputs.count) return false;
        for (int i = 0; i < a->proc.inputs.count; i++) {
            if (!type_eq(&a->proc.inputs.data[i], &b->proc.inputs.data[i])) {
                return false;
            }
        }
        for (int i = 0; i < a->proc.outputs.count; i++) {
            if (!type_eq(&a->proc.outputs.data[i], &b->proc.outputs.data[i])) {
                return false;
            }
        }
        return true;
    default:
        fprintf(stderr, "Warning: Cannot type check unknown type connective "
            "%d.\n", a->connective);
        return true;
    }
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

    OP_CALL,
    OP_RET,

    OP_ARRAY_ALLOC,
    OP_ARRAY_OFFSET,
    OP_ARRAY_STORE,
    OP_ARRAY_INDEX,
    OP_ARRAY_CONCAT,
    OP_DECREMENT_REFCOUNT, /* May be redundant with OP_MOV to REF_NULL */

    /* Stack operations, for allocating/freeing tuples and records. */
    OP_STACK_ALLOC,
    OP_STACK_FREE,
    /* Pointer operations, for manipulating tuples and records. */
    OP_POINTER_OFFSET, /* Like add, but don't discard arg1. */
    OP_POINTER_STORE,
    OP_POINTER_COPY,
    OP_POINTER_COPY_OVERLAPPING,
    OP_POINTER_LOAD,
    OP_POINTER_INCREMENT_REFCOUNT,
    OP_POINTER_DECREMENT_REFCOUNT,
};

enum operation_flags {
    OP_8BIT  = 0x0,
    OP_16BIT = 0x1,
    OP_32BIT = 0x2,
    OP_64BIT = 0x3,

    OP_FLOAT = 0x4, /* This is a mask, not a valid value on its own. */
    OP_FLOAT32 = 0x6,
    OP_FLOAT64 = 0x7,

    OP_SHARED_BUFF = 0x8, /* TODO: masks for lifted storage of small arrays? */
};

enum ref_type {
    REF_NULL,
    REF_CONSTANT,
    REF_STATIC_POINTER, /* Does this actually need to be different? */
    REF_GLOBAL,
    REF_LOCAL,
    /* This uses the same indices as REF_LOCAL, but some instructions behave
       differently when given REF_TEMPORARY refs. A better name might be
       REF_MOVE or something. */
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

#endif
