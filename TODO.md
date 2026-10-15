# TODO: bytecode compiler to oracle parity

Oracle: `craftinginterpreters/c/` + `test/` (book suite, `clox` language).
Seams: CLI binary (`./main` exit/stdout/stderr per .lox) + table.c unit tests.
Harness: extend `/tmp/opencode/cgen_runtests.py` to the bytecode path.

- [x] Slice 1 — statements: OP_PRINT, OP_POP, `;`; OP_RETURN stops printing; REPL echo dropped; debug.c coverage. Red: test/print/* (test/expressions/evaluate.lox excluded: skipped in the clox suite). Green: 97 PASS suite-wide, 0 in-scope failures. Incidental parity: debug flags off in build (mirror oracle omit), "Expect expression." period, `assert(0)`→`return`, `#undef match` for datatype99 collision.
- [x] Slice 2 — table.c/h + unit tests (/tmp/opencode/table_test.c); OP_DEFINE/GET/SET_GLOBAL; vm.globals/strings; identifier interning (ObjString.hash, FNV-1a); canAssign plumbing + namedVariable/varDeclaration. Red→green: 5/5 targets (redeclare/redefine/global/multiline/use_global_in_initializer); suite 97→109, 0 in-scope failures. Runner mirrors clox suite skips (benchmark/scanning/expressions).
- [x] Slice 3 — Compiler struct, locals, scope depth; OP_GET/SET_LOCAL (+ byteInstruction). Red→green: 11/14 targets (early_bound needs fun, block/empty needs if); suite 109→118, 0 in-scope failures (all 128 remaining first-errors classify to slices 4–8; `{` gone). Incidental: UINT8_COUNT in common.h.
- [x] Slice 4 — OP_JUMP/JUMP_IF_FALSE/LOOP (+ emitJump/patchJump/emitLoop, jumpInstruction); if/else, while, for, and/or. Red→green: 25/32 targets (7 remaining need fun/return/closure: for+while closure_in_body/return_closure/return_inside, for/syntax); suite 118→130, 0 in-scope failures.
- [x] Slice 5 — frames (CallFrame/FRAMES_MAX), ObjFunction/ObjNative (+ printObject/freeObject), OP_CALL, OP_RETURN rework, clock()/defineNative; compile() returns ObjFunction*; call/callValue, funDeclaration/returnStatement/call/argumentList, valuesEqual→pointer equality. Red→green: function/*, call/* (minus call/object: needs class), return/* (minus in_method); suite 130→~165, 0 in-scope failures.
- [x] Slice 6 — upvalues (OP_GET/SET_UPVALUE, OP_CLOSURE, OP_CLOSE_UPVALUE, captureUpvalue/closeUpvalues, resolveUpvalue/addUpvalue, markCompilerRoots); mark-sweep GC (markObject/markValue/markTable/tableRemoveWhite/collectGarbage, concatenate/addConstant GC-safety). Red→green: closure/* (minus close_over_method_parameter), limit/*, for/syntax, while/*, operator/not, regression/40, variable/early_bound; suite →172, 0 in-scope failures (all 74 remaining first-errors need class/method/field/inheritance: slices 7–8).
- [ ] Slice 7 — ObjClass/Instance/BoundMethod; OP_CLASS/METHOD/INVOKE/GET/SET_PROPERTY; initString. Red: class/*, method/*, field/*, constructor/*, this/*
- [ ] Slice 8 — OP_INHERIT/GET_SUPER/SUPER_INVOKE. Red: inheritance/*, super/*
- [ ] Slice 9 — CLI contract (usage exit 64, missing-file 74); FULL suite green.
