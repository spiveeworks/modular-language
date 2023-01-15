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

    while (true) {
        int prev_count = program.count;
        parse_statement(
            &program,
            &tokenizer,
            &bindings
        );

        printf("\nStatement parsed. Output:\n");
        for (int i = prev_count; i < program.count; i++) {
            struct instruction *instr = &program.data[i];
            if (instr->op == OP_MOV) {
                print_ref(instr->output);
                printf(" = ", instr->op);
                print_ref(instr->arg1);
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
    }

    return 0;
}

