#ifndef clox_cgen_h
#define clox_cgen_h

// Nanopass-style AST->C compiler for Lox without OOP (no classes).
//
// Passes (each single-task, in order):
//   check-core     L_src  -> L_core or error   (rejects Class/Get/Set/Super/
//                                              This; Return only inside fun)
//   resolve-scopes L_core -> L_res             (unique _lox_<name>_<uid>;
//    top-level fun names hoisted for forward refs; same-scope dups and
//    self-initializers rejected like the VM; capture analysis for closures)
//   lower-logical  L_res  -> L_flat            (Logical -> temp + If;
//                                              Call/Function/Return recursed)
//   emit-c         L_flat -> C text            (plain C calling loxrt; every
//    Lox function becomes a file-scope C function + ObjCFun value, captured
//    variables live in heap cells, script globals carry defined-flags)
//
//
// All output trees and the output text are built with arena astr in THIS
// implementation; the GENERATED C is plain code with loxrt helper calls.

#include <stdbool.h>

#include <arena.h>

#include "parser.h"

typedef struct {
  astr code;
  bool ok;
} CgenResult;

// Compile a parsed program to C source text. Prints "[line N] Error: ..."
// and returns ok=false on unsupported constructs or undefined variables.
CgenResult cgen_compile(Arena* arena, StmtList* prog);

#endif
