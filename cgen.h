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
// All IR is built with arena astr in THIS implementation; the output text is
// streamed to the caller and the GENERATED C is plain code with loxrt helper
// calls.

#include <stdbool.h>
#include <stdio.h>

#include "mmap_arena.h"

#include "parser.h"

// Compile a parsed program to C source text, streaming it to `out`
// incrementally (no retained output buffer) so peak RSS stays proportional
// to the IR, not to the generated text. Prints "[line N] Error: ..." and
// returns false on unsupported constructs or compile errors. All checking
// (check-core, resolve-scopes, lower-logical) completes before the first
// byte is written, so a failed check/resolve/lower writes nothing.
//
// Lifetime: `prog`, the source text, and all generated IR live in `arena`.
// The caller resets the arena before the next input and releases it when
// done; this function neither resets nor releases it.
bool cgen_write_c(FILE* out, Arena* arena, StmtList* prog);

#endif
