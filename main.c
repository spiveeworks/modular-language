#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "types.h"

#include "compiler_primitives.h"
#include "tokenizer.h"
#include "expressions.h"
#include "statements.h"
#include "interpreter.h"

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
    if (ref.type == REF_STATIC_POINTER) {
        printf("0x%p", (void*)ref.x);
        return;
    }

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
    struct call_stack call_stack = {0};

    while (true) {
        struct item item = parse_item(&tokenizer, &bindings);

        if (item.type == ITEM_STATEMENT) {
            printf("\nStatement parsed. Output:\n");
            for (int i = 0; i < item.statement_code.count; i++) {
                struct instruction *instr = &item.statement_code.data[i];
                if (instr->op == OP_MOV) {
                    print_ref(instr->output);
                    printf(" = ");
                    print_ref(instr->arg1);
                    printf("\n");
                } else if (instr->op == OP_ARRAY_ALLOC) {
                    print_ref(instr->output);
                    printf(" = alloc_array(");
                    print_ref(instr->arg1);
                    printf(", ");
                    print_ref(instr->arg2);
                    printf(")\n");
                } else if (instr->op == OP_ARRAY_STORE) {
                    print_ref(instr->output);
                    printf("[", instr->op);
                    print_ref(instr->arg1);
                    printf("] = ");
                    print_ref(instr->arg2);
                    printf("\n");
                } else {
                    print_ref(instr->output);
                    printf(" = Op%d ", instr->op);
                    print_ref(instr->arg1);
                    printf(", ");
                    print_ref(instr->arg2);
                    printf("\n");
                }
            }

            printf("\nExecuting.\n");
            execute_top_level_code(&call_stack, &item.statement_code);
            if (call_stack.vars.global_count != bindings.global_count) {
                fprintf(stderr, "Warning: Executing a statement resulted in "
                    "%llu global variables being initialized, when %llu "
                    "global variables are in scope.\n",
                    call_stack.vars.global_count, bindings.global_count);
                call_stack.vars.global_count = bindings.global_count;
            }
        } else if (item.type == ITEM_NULL) {
            break;
        } else {
            fprintf(stderr, "Error: Unknown item type %d?\n", item.type);
        }
    }

    printf("\nResults:\n");
    for (int i = 0; i < call_stack.vars.global_count; i++) {
        struct variable_data *it = &call_stack.vars.data[i];
        printf("g%d = %lld\n", i, (int64)it->value.val64);
    }

    return 0;
}

