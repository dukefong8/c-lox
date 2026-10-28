
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgen.h"
#include "common.h"
#include "mmap_arena.h"
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

// Verify `path` opens, so `--emit-c` can reject a bad input before writing
// any output. Uses the same diagnostic as readFile.
static void checkReadable(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }
  fclose(file);
}

// Read `path` into `arena`, resetting it first. The arena is a fixed virtual
// reservation; reading commits only the pages the source actually occupies.
static char* readFile(Arena* arena, const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }

  fseek(file, 0L, SEEK_END);
  long end = ftell(file);
  if (end < 0) {
    fprintf(stderr, "Could not read file \"%s\".\n", path);
    exit(74);
  }
  size_t fileSize = (size_t)end;
  rewind(file);

  arena_reset(arena);

  size_t need = fileSize + 1;

  char* buffer = (char*)New(arena, char, need, NO_INIT);
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
  // Reserve the compiler arena range up front; pages commit as the source is
  // read, so no input length has to be known beforehand.
  Arena sourceArena = arena_init(NULL, clox_arena_reserve());
  jmp_buf oom;
  if (ArenaOOM(&sourceArena, oom)) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  char* source = readFile(&sourceArena, path);
  InterpretResult result = interpret(source);
  // parse(&sourceArena, source);
  arena_release(&sourceArena);

  if (result == INTERPRET_COMPILE_ERROR) exit(65);
  if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

// Output path for `sourcePath`: same directory, `.lox` replaced by `.c`
// (or `.c` appended when the source has no `.lox` suffix).
static char* outputPath(const char* sourcePath) {
  size_t n = strlen(sourcePath);
  size_t stem = n;
  if (n >= 4 && strcmp(sourcePath + n - 4, ".lox") == 0)
    stem = n - 4;

  char* out = (char*)malloc(stem + 3);  // ".c" + NUL
  if (out == NULL) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(out, sourcePath, stem);
  memcpy(out + stem, ".c", 3);
  return out;
}

static char* tempPath(const char* outPath) {
  size_t n = strlen(outPath);
  char* tmp = (char*)malloc(n + 5);  // ".tmp" + NUL
  if (tmp == NULL) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memcpy(tmp, outPath, n);
  memcpy(tmp + n, ".tmp", 5);
  return tmp;
}

// Compile one input to C next to its source. The source, AST, and cgen IR
// share `arena`, which `readFile` resets for this input; the next input's
// `readFile` resets it again, and the driver releases it at the end. Output
// is written to a temporary and renamed on success, so a failed compile
// leaves an existing `.c` untouched.
static int emitCFile(Arena* arena, const char* path) {
  char* source = readFile(arena, path);

  StmtList* statements = New(arena, StmtList);
  ParseResult pres = parseProgram(arena, source, statements);
  if (pres.hadError)
    return 65;

  char* outPath = outputPath(path);
  char* tmpPath = tempPath(outPath);
  FILE* out = fopen(tmpPath, "wb");
  if (out == NULL) {
    fprintf(stderr, "Could not write file \"%s\".\n", outPath);
    free(tmpPath);
    free(outPath);
    exit(74);
  }

  bool ok = cgen_write_c(out, arena, statements);
  bool writeError = fclose(out) != 0;
  if (writeError) {
    fprintf(stderr, "Could not write file \"%s\".\n", outPath);
    remove(tmpPath);
    free(tmpPath);
    free(outPath);
    exit(74);
  }
  if (!ok) {
    remove(tmpPath);
    free(tmpPath);
    free(outPath);
    return 65;
  }
  if (rename(tmpPath, outPath) != 0) {
    fprintf(stderr, "Could not write file \"%s\".\n", outPath);
    remove(tmpPath);
    free(tmpPath);
    free(outPath);
    exit(74);
  }
  free(tmpPath);
  free(outPath);
  return 0;
}

static int emitCFiles(int count, char** paths) {
  // Reject unreadable inputs before compiling anything, so a missing path
  // leaves no partial `.c` behind.
  for (int i = 0; i < count; i++)
    checkReadable(paths[i]);

  // One arena for the whole run, reset per input. Its range is reserved
  // virtually and pages commit on demand, so no per-input sizing is needed.
  Arena arena = arena_init(NULL, clox_arena_reserve());
  jmp_buf oom;
  if (ArenaOOM(&arena, oom)) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  int exitCode = 0;
  for (int i = 0; i < count; i++) {
    if (emitCFile(&arena, paths[i]) != 0)
      exitCode = 65;
  }

  arena_release(&arena);
  return exitCode;
}

int main(int argc, char* argv[]) {
  initVM();

  int exitCode = 0;
  if (argc == 1) {
    repl();
  } else if (argc >= 3 && strcmp(argv[1], "--emit-c") == 0) {
    exitCode = emitCFiles(argc - 2, &argv[2]);
  } else if (argc == 2) {
    runFile(argv[1]);
  } else {
    fprintf(stderr, "Usage: clox [path] [--emit-c path...]\n");
  }

  freeVM();
  return exitCode;
}
