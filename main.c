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

void print_data(uint8 *it, struct type *type) {
    if (type->connective == TYPE_ARRAY) {
        struct shared_buff *buff = (struct shared_buff*)it;
        struct type *element_type = type->inner;
        printf("[");
        if (buff->count > 0) {
            uint8 *data = shared_buff_get_index(*buff, 0);
            for (int i = 0; i < buff->count; i++) {
                if (i > 0) printf(", ");

                print_data(data, element_type);

                data += element_type->total_size;
            }
        }
        printf("]");
    } else if (type->connective == TYPE_INT) {
        int64 *as_int = (int64*)it;
        printf("%lld", *as_int);
    } else if (type->connective == TYPE_TUPLE) {
        printf("{");
        for (int i = 0; i < type->elements.count; i++) {
            if (i > 0) printf(", ");
            struct type *elem_ty = &type->elements.data[i];
            print_data(it, elem_ty);
            it += elem_ty->total_size;
        }
        printf("}");
    }
}

void print_call_stack_value(union variable_contents it, struct type *type) {
    if (type->connective == TYPE_TUPLE || type->connective == TYPE_RECORD) {
        print_data(it.pointer, type);
    } else {
        print_data(it.bytes, type);
    }
}

struct statement {
    struct instruction_buffer instructions;
    struct intermediate_buffer intermediates;
};

struct statement_buffer {
    struct statement *data;
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
    struct procedure_buffer procedures = {0};
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
            struct statement statement;
            statement.instructions = item.instructions;
            statement.intermediates = item.intermediates;
            buffer_push(statements, statement);

            if (debug) {
                printf("\nStatement parsed. Output:\n");
                disassemble_instructions(item.instructions);
            }
        } else if (item.type == ITEM_PROCEDURE) {
            struct procedure *p = buffer_addn(procedures, 1);
            p->instructions = item.instructions;

            buffer_push(bindings, item.proc_binding);
            bindings.global_count = bindings.count;

            struct variable_data *var = buffer_addn(call_stack.vars, 1);
            var->mem_mode = VARIABLE_DIRECT_VALUE;
            var->value.val64 = procedures.count - 1;
            call_stack.vars.global_count = bindings.global_count;
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
        struct intermediate_buffer results = {0};
        for (int i = 0; i < statements.count; i++) {
            struct statement *it = &statements.data[i];
            execute_top_level_code(procedures, &call_stack, &it->instructions);
            /* TODO: Check vars.global_count after each statement? */

            /* Top level statements are fired once and then forgotten. */
            buffer_free(it->instructions);

            /* Overwrite results with this statement. At the end of the loop we
               will be left with the last statement's results. */
            buffer_free(results);
            results = it->intermediates;
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
            print_call_stack_value(call_stack.vars.data[i].value, &bindings.data[i].type);
            printf("\n");
        }

        if (repl && results.count > 0) {
            /* If the last statement in this line was a bare expression, print
               its results. */
            printf("result = ");
            for (int i = 0; i< results.count; i++) {
                if (i > 0) printf(", ");

                struct intermediate *it = &results.data[i];
                /* Kinda hacky, just reuse the interpreter's read_ref function,
                   but with a bogus execution frame representing the fact that
                   there are no local variables. */
                struct execution_frame frame = {.locals_count = call_stack.vars.global_count};
                union variable_contents val = read_ref(
                    &frame,
                    &call_stack.vars,
                    it->ref,
                    NULL
                );
                print_call_stack_value(val, &it->type);
            }
            printf("\n");
        }

        buffer_free(results);
        unbind_temporaries(&call_stack.vars);

        if (repl) printf("> ");
    }

    return 0;
}

