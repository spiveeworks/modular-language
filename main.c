#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "buffer.h"
#include "types.h"

#include "compiler_primitives.h"
#include "tokenizer.h"
#include "expressions.h"
#include "statements.h"
#include "interpreter.h"

void print_ref(struct ref ref) {
    switch (ref.type) {
      case REF_STATIC_POINTER:
        printf("0x%p", (void*)ref.x);
        return;
      case REF_CONSTANT:
        printf(" %lld", (long long)ref.x);
        break;
      case REF_GLOBAL:
        printf("g%lld", (long long)ref.x);
        break;
      case REF_LOCAL:
        printf("l%lld", (long long)ref.x);
        break;
      case REF_TEMPORARY:
        printf("v%lld", (long long)ref.x);
        break;
      default:
        break;
    }
}

void print_array(struct shared_buff buff) {
    printf("[");
    if (buff.ptr) {
        struct type *element_type = buff.ptr->element_type;
        switch (element_type->connective) {
        case TYPE_INT:
          {
            int64 *arr = shared_buff_get_index(buff, 0);
            for (int i = 0; i < buff.count; i++) {
                if (i > 0) printf(", ");
                printf("%lld", (long long)arr[i]);
            }
            break;
          }
        case TYPE_ARRAY:
          {
            struct shared_buff *arr = shared_buff_get_index(buff, 0);
            for (int i = 0; i < buff.count; i++) {
                if (i > 0) printf(", ");
                print_array(arr[i]);
            }
            break;
          }
        default:
            for (int i = 0; i < buff.count; i++) {
                if (i > 0) printf(", ");
                printf("?");
            }
        }
    }
    printf("]");
}

void disassemble_instructions(struct instruction_buffer instructions) {
    for (int i = 0; i < instructions.count; i++) {
        struct instruction *instr = &instructions.data[i];
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
            printf("[");
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
}

void print_call_stack_value(struct variable_data *it) {
    if (it->mem_mode == VARIABLE_REFCOUNT) {
        print_array(it->value.shared_buff);
    } else {
        printf("%lld", (long long)it->value.val64);
    }
}

struct statement_buffer {
    struct instruction_buffer *data;
    size_t count;
    size_t capacity;
};

bool debug = false;

int main(int argc, char **argv) {
    char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-debug") == 0) {
            if (debug) {
                fprintf(stderr, "Warning: Got -debug option multiple times. "
                    "Ignoring.\n");
            }
            debug = true;
        } else {
            if (input_path) {
                fprintf(stderr, "Error: Got too many command line "
                    "arguments.\n");
                exit(EXIT_FAILURE);
            }
            input_path = argv[i];
        }
    }

    FILE *input;
    bool repl;
    if (input_path) {
        input = fopen(input_path, "rb");
        if (!input) {
            fprintf(stderr, "Error: couldn't open file \"%s\"\n", input_path);
            exit(EXIT_FAILURE);
        }
        repl = false;
    } else {
        input = stdin;
        repl = true;
    }

    struct tokenizer tokenizer = start_tokenizer(input);
    if (repl) {
        printf("Unmatched Perspicacity Prompt\n");
        printf("> ");
    }
    struct record_table bindings = {0};
    struct call_stack call_stack = {0};

    struct statement_buffer statements = {0};

    while (true) {
        if (repl) {
            /* We just displayed a prompt. Before we try parsing anything, just
               skip lines of user input that are all whitespace. */
            while (tokenizer_try_read_eol(&tokenizer)) {
                printf("> ");
            }
        }

        struct item item = parse_item(&tokenizer, &bindings, repl);

        if (item.type == ITEM_STATEMENT) {
            buffer_push(statements, item.statement_code);

            if (debug) {
                printf("\nStatement parsed. Output:\n");
                disassemble_instructions(item.statement_code);
            }
        } else if (item.type == ITEM_NULL) {
            break;
        } else {
            fprintf(stderr, "Error: Unknown item type %d?\n", item.type);
            exit(EXIT_FAILURE);
        }

        /* Keep parsing until a statement or expression aligns with the end of
           a line of user input. */
        if (repl && !tokenizer_try_read_eol(&tokenizer)) continue;

        /* Finished parsing something. Time to execute it. */
        /* Store global count to track how many are new. */
        int prev_global_count = call_stack.vars.global_count;

        if (debug) printf("\nExecuting.\n");
        for (int i = 0; i < statements.count; i++) {
            execute_top_level_code(&call_stack, &statements.data[i]);

            /* TODO: Check vars.global_count after each statement? */

            /* Top level statements are fired once and then forgotten. */
            buffer_free(statements.data[i]);
        }
        /* Empty the statement buffer, and reuse it next loop. */
        statements.count = 0;

        if (call_stack.vars.global_count != bindings.global_count) {
            fprintf(stderr, "Warning: Executing statements resulted in "
                    "%llu global variables being initialized, when %llu "
                    "global variables are in scope.\n",
                    (long long)call_stack.vars.global_count,
                    (long long)bindings.global_count);
            call_stack.vars.global_count = bindings.global_count;
        }

        if (debug && prev_global_count < call_stack.vars.global_count) {
            printf("\nState:\n");
        }
        for (int i = prev_global_count; i < call_stack.vars.global_count; i++) {
            fputstr(bindings.data[i].name, stdout);
            printf(" = ");
            print_call_stack_value(&call_stack.vars.data[i]);
            printf("\n");
        }

        if (repl) {
            /* If the last statement in this line was a bare expression, print
               its results. */
            size_t start = call_stack.vars.global_count;
            size_t end = call_stack.vars.count;
            if (start < end) {
                printf("result = ");
                for (size_t i = start; i < end; i++) {
                    if (i > start) printf(", ");

                    struct variable_data *it = &call_stack.vars.data[i];

                    print_call_stack_value(it);
                }
                printf("\n");
            }
        }
        unbind_temporaries(&call_stack.vars);

        if (repl) printf("> ");
    }

    return 0;
}

