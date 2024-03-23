#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#define ARGPARSE_IMPLEMENTATION
#include "assembler.h"
#include "codegen.h"
#include "ds.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"

// Add support for the following:
// - abort for dispatch on void
// - abort for case on void
// - abort for case on no match
// - exception handling
//
// Future plans:
// - add a new class Linux for the syscalls and implement a prelude for it
// - implement a better main/build system
// - add graphics to IO

enum status_code {
    STATUS_OK = 0,
    STATUS_ERROR = 1,
    STATUS_STOP = 2,
};

typedef struct build_context {
    ds_argparse_parser parser;
    ds_dynamic_array prelude_filepaths; // const char *
    ds_dynamic_array user_filepaths; // const char *
    ds_dynamic_array asm_filepaths; // const char *

    ds_dynamic_array user_programs; // program_node
    program_node program;
    semantic_mapping mapping;
} build_context;

static int build_context_prelude_init(build_context *context) {
    int result = 0;
    char *cool_home = NULL;
    char *cool_lib = NULL;
    ds_dynamic_array filepaths;

    cool_home = getenv("COOL_HOME");
    if (cool_home == NULL) {
        cool_home = ".";
    }

    if (util_append_path(cool_home, "/lib", &cool_lib) != 0) {
        DS_LOG_ERROR("Failed to append path");
        return_defer(1);
    }

    if (util_list_filepaths(cool_lib, &filepaths) != 0) {
        DS_LOG_ERROR("Failed to list filepaths");
        return_defer(1);
    }

    for (size_t i = 0; i < filepaths.count; i++) {
        char *filepath = NULL;
        ds_dynamic_array_get(&filepaths, i, (void **)&filepath);

        ds_string_slice slice, ext;
        ds_string_slice_init(&slice, filepath, strlen(filepath));
        while (ds_string_slice_tokenize(&slice, '.', &ext) == 0) { }
        char *extension = NULL;
        ds_string_slice_to_owned(&ext, &extension);

        if (strcmp(extension, "cl") == 0) {
            if (ds_dynamic_array_append(&context->prelude_filepaths, &filepath) != 0) {
                DS_LOG_ERROR("Failed to append filepath");
                return_defer(1);
            }
        } else if (strcmp(extension, "asm") == 0) {
            if (ds_dynamic_array_append(&context->asm_filepaths, &filepath) != 0) {
                DS_LOG_ERROR("Failed to append filepath");
                return_defer(1);
            }
        }
    }

defer:
    return result;
}

static int build_context_init(build_context *context, ds_argparse_parser parser) {
    int result = 0;

    context->parser = parser;

    ds_dynamic_array_init(&context->prelude_filepaths, sizeof(const char *));
    ds_dynamic_array_init(&context->user_filepaths, sizeof(const char *));
    ds_dynamic_array_init(&context->asm_filepaths, sizeof(const char *));

    if (build_context_prelude_init(context) != 0) {
        DS_LOG_ERROR("Failed to initialize prelude");
        return_defer(1);
    }

    ds_dynamic_array_init(&context->user_programs, sizeof(program_node));
    ds_dynamic_array_init(&context->program.classes, sizeof(class_node));
    context->mapping = (struct semantic_mapping){0};

defer:
    return result;
}

static enum status_code parse_prelude(build_context *context) {
    int length;
    program_node program;
    char *buffer = NULL;
    ds_dynamic_array tokens; // struct token

    enum parser_result parser_status = PARSER_OK;

    enum status_code result = STATUS_OK;

    for (size_t i = 0; i < context->prelude_filepaths.count; i++) {
        const char *filepath = NULL;
        ds_dynamic_array_get(&context->prelude_filepaths, i, (void **)&filepath);

        length = util_read_file(filepath, &buffer);
        if (length < 0) {
            DS_LOG_ERROR("Failed to read file: %s", filepath);
            return_defer(STATUS_ERROR);
        }

        // tokenize prelude
        ds_dynamic_array_init(&tokens, sizeof(struct token));
        if (lexer_tokenize(buffer, length, &tokens) != LEXER_OK) {
            DS_LOG_ERROR("Failed to tokenize input");
            return_defer(STATUS_ERROR);
        }

        // parse tokens
        if (parser_run(filepath, &tokens, &program) != PARSER_OK) {
            parser_status = PARSER_ERROR;
            continue;
        }

        for (unsigned int j = 0; j < program.classes.count; j++) {
            class_node *c = NULL;
            ds_dynamic_array_get_ref(&program.classes, j, (void **)&c);

            if (ds_dynamic_array_append(&context->program.classes, c) != 0) {
                DS_LOG_ERROR("Failed to append class");
                return_defer(1);
            }
        }
    }

    if (parser_status != PARSER_OK) {
        return_defer(STATUS_ERROR);
    }

    return_defer(STATUS_OK);

defer:
    return result;
}

static enum status_code parse_user(build_context *context) {
    int length;
    program_node program;
    char *buffer = NULL;
    ds_dynamic_array tokens; // struct token

    int lexer_stop = ds_argparse_get_flag(&context->parser, ARG_LEXER);
    int parser_stop = ds_argparse_get_flag(&context->parser, ARG_SYNTAX);

    enum parser_result parser_status = PARSER_OK;

    int result = STATUS_OK;

    for (size_t i = 0; i < context->user_filepaths.count; i++) {
        const char *filepath = NULL;
        ds_dynamic_array_get(&context->user_filepaths, i, (void **)&filepath);

        // read input file
        length = util_read_file(filepath, &buffer);
        if (length < 0) {
            DS_LOG_ERROR("Failed to read file: %s", filepath);
            return_defer(STATUS_ERROR);
        }

        // tokenize input
        ds_dynamic_array_init(&tokens, sizeof(struct token));
        if (lexer_tokenize(buffer, length, &tokens) != LEXER_OK) {
            DS_LOG_ERROR("Failed to tokenize input");
            return_defer(STATUS_ERROR);
        }

        if (lexer_stop == 1) {
            lexer_print_tokens(&tokens);
            continue;
        }

        // parse tokens
        program_node program;
        if (parser_run(filepath, &tokens, &program) != PARSER_OK) {
            parser_status = PARSER_ERROR;
            continue;
        }

        for (unsigned int j = 0; j < program.classes.count; j++) {
            class_node *c = NULL;
            ds_dynamic_array_get_ref(&program.classes, j, (void **)&c);

            if (ds_dynamic_array_append(&context->program.classes, c) != 0) {
                DS_LOG_ERROR("Failed to append class");
                return_defer(STATUS_ERROR);
            }
        }

        if (ds_dynamic_array_append(&context->user_programs, &program) != 0) {
            DS_LOG_ERROR("Failed to append program");
            return_defer(1);
        }
    }

    if (lexer_stop == 1) {
        return_defer(STATUS_STOP);
    }

    if (parser_status != PARSER_OK) {
        return_defer(STATUS_ERROR);
    }

    if (parser_stop == 1) {
        for (size_t i = 0; i < context->user_programs.count; i++) {
            program_node *program = NULL;
            ds_dynamic_array_get_ref(&context->user_programs, i, (void **)&program);
            parser_print_ast(program);
        }
        return_defer(STATUS_STOP);
    }

    return_defer(STATUS_OK);

defer:
    return result;
}

static enum status_code gatekeeping(build_context *context) {
    int semantic_stop = ds_argparse_get_flag(&context->parser, ARG_SEMANTIC);
    int mapping_stop = ds_argparse_get_flag(&context->parser, ARG_MAPPING);

    int result = STATUS_OK;

    if (semantic_check(&context->program, &context->mapping) != SEMANTIC_OK) {
        return_defer(STATUS_ERROR);
    }

    if (semantic_stop == 1) {
        for (size_t i = 0; i < context->user_programs.count; i++) {
            program_node *program = NULL;
            ds_dynamic_array_get_ref(&context->user_programs, i, (void **)&program);
            parser_print_ast(program);
        }
        return_defer(STATUS_STOP);
    }

    if (mapping_stop == 1) {
        semantic_print_mapping(&context->mapping);
        return_defer(STATUS_STOP);
    }

    return_defer(STATUS_OK);

defer:
    return result;
}

static enum status_code codegen(build_context *context) {
    int length;
    char *buffer = NULL;

    char *output = ds_argparse_get_value(&context->parser, ARG_OUTPUT);
    int tacgen_stop = ds_argparse_get_flag(&context->parser, ARG_TACGEN);
    int assembler_stop = ds_argparse_get_flag(&context->parser, ARG_ASSEMBLER);

    int result = STATUS_OK;

    if (tacgen_stop == 1) {
        for (size_t i = 0; i < context->user_programs.count; i++) {
            program_node *program = NULL;
            ds_dynamic_array_get_ref(&context->user_programs, i, (void **)&program);
            codegen_tac_print(&context->mapping, program);
        }
        return_defer(STATUS_STOP);
    }

    for (size_t i = 0; i < context->asm_filepaths.count; i++) {
        const char *asm_filepath = NULL;
        ds_dynamic_array_get(&context->asm_filepaths, i, (void **)&asm_filepath);

        // read asm prelude file
        length = util_read_file(asm_filepath, &buffer);
        if (length < 0) {
            DS_LOG_ERROR("Failed to read file: %s", asm_filepath);
            return_defer(STATUS_ERROR);
        }

        if (util_write_file(output, buffer) != 0) {
            DS_LOG_ERROR("Failed to write file: %s", output);
            return_defer(STATUS_ERROR);
        }
    }

    // assembler
    if (assembler_run(output, &context->mapping) != ASSEMBLER_OK) {
        return_defer(STATUS_ERROR);
    }

    if (assembler_stop == 1) {
        return_defer(STATUS_STOP);
    }

    return_defer(STATUS_OK);

defer:
    return result;
}

int main(int argc, char **argv) {
    int result = 0;
    build_context context;
    ds_argparse_parser parser;

    if (util_parse_arguments(&parser, argc, argv) != 0) {
        DS_LOG_ERROR("Failed to parse arguments");
        return_defer(1);
    }

    build_context_init(&context, parser);

    // TODO: Take from args
    char *filename = ds_argparse_get_value(&parser, ARG_INPUT);
    ds_dynamic_array_append(&context.user_filepaths, &filename);

    int prelude_result = parse_prelude(&context);
    int user_result = parse_user(&context);
    if (prelude_result == STATUS_STOP || user_result == STATUS_STOP) {
        return_defer(0);
    }
    if (prelude_result != STATUS_OK || user_result != STATUS_OK) {
        printf("Compilation halted\n");
        return_defer(1);
    }

    int gatekeeping_result = gatekeeping(&context);
    if (gatekeeping_result == STATUS_STOP) {
        return_defer(0);
    }
    if (gatekeeping_result != STATUS_OK) {
        printf("Compilation halted\n");
        return_defer(1);
    }

    int codegen_result = codegen(&context);
    if (codegen_result == STATUS_STOP) {
        return_defer(0);
    }
    if (codegen_result != STATUS_OK) {
        printf("Compilation halted\n");
        return_defer(1);
    }

    return_defer(0);

defer:
    return result;
}
