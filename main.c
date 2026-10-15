
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arena.h>

#include "cgen.h"
#include "common.h"
#include "parser.h"
#include "vm.h"

static void repl() {
  char line[1024];
  for (;;) {
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    interpret(line);
  }
}

static char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }

  fseek(file, 0L, SEEK_END);
  size_t fileSize = ftell(file);
  rewind(file);

  char* buffer = (char*)malloc(fileSize + 1);
  size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
  if (bytesRead < fileSize) {
    fprintf(stderr, "Could not read file \"%s\".\n", path);
    exit(74);
  }

  buffer[bytesRead] = '\0';

  fclose(file);
  return buffer;
}

static void runFile(const char* path) {
  char* source = readFile(path);
  InterpretResult result = interpret(source);
  // parse(source);
  free(source);

  if (result == INTERPRET_COMPILE_ERROR) exit(65);
  if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

static void emitCFile(const char* path) {
  char* source = readFile(path);

  // Parsing and codegen are pure functions of (arena, source), so an
  // exhausted arena is retried with a doubled buffer instead of failing:
  // the 16 MB start is a guess, not a limit. Past the cap, treat it as
  // genuine exhaustion (exit 1, like the oracle on malloc failure).
  size_t size = MB(16);
  for (;;) {
    byte* buf = malloc(size);
    if (buf == NULL) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    Arena arena = arena_init(buf, size);

    jmp_buf oom;
    if (ArenaOOM(&arena, oom)) {
      free(buf);
      size *= 2;
      if (size > MB(256)) {
        fprintf(stderr, "Out of memory.\n");
        free(source);
        exit(1);
      }
      continue;
    }

    int exitCode = 0;
    ParseResult pres = parseProgram(&arena, source);
    if (pres.hadError) {
      exitCode = 65;
    } else {
      CgenResult cres = cgen_compile(&arena, pres.statements);
      if (!cres.ok) {
        exitCode = 65;
      } else {
        fwrite(cres.code.data, 1, (size_t)cres.code.len, stdout);
      }
    }

    free(source);
    free(buf);
    if (exitCode != 0) exit(exitCode);
    return;
  }
}

int main(int argc, char* argv[]) {
  initVM();

  if (argc == 1) {
    repl();
  } else if (argc == 3 && strcmp(argv[1], "--emit-c") == 0) {
    emitCFile(argv[2]);
  } else if (argc == 2) {
    runFile(argv[1]);
  } else {
    fprintf(stderr, "Usage: clox [path] [--emit-c path]\n");
  }

  freeVM();
  return 0;
}
