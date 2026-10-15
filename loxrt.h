#ifndef clox_loxrt_h
#define clox_loxrt_h

// Runtime helpers for C code emitted by cgen. Operates on the reused
// Value/Obj runtime (value.c, object.c, memory.c). Error messages and the
// exit(70) code mirror vm.c.

#include <stdbool.h>

#include "object.h"
#include "value.h"

void lox_init(void);
void lox_free(void);

// String literal: copies chars (like compiler.c:string via copyString).
Value lox_string(const char* chars, int length);

// Truthiness: mirrors the interpreter's isFalsey.
bool lox_is_falsey(Value value);

// Operators. `line` feeds runtime-error messages; type errors print to
// stderr and exit(70).
Value lox_add(Value a, Value b, int line);
Value lox_sub(Value a, Value b, int line);
Value lox_mul(Value a, Value b, int line);
Value lox_div(Value a, Value b, int line);
Value lox_negate(Value value, int line);
Value lox_not(Value value);
Value lox_equal(Value a, Value b);
Value lox_greater(Value a, Value b, int line);
Value lox_less(Value a, Value b, int line);

void lox_print(Value value);

// Heap cell for a captured (closed-over) variable. Cells outlive the C
// frame that created them; closures share them by pointer.
typedef struct {
  Value value;
} LoxCell;

LoxCell* lox_newcell(Value value);

// Call a Lox-callable Value (CFun or native) with argCount arguments.
// Arity mismatches and non-callables abort with the VM-identical message
// (exit 70). `line` is the call-site line.
Value lox_call(Value callee, Value* args, int argCount, int line);

// Arity failure: mirrors the VM's call() message (exit 70).
void lox_arity(int line, int want, int got);

// The clock native as a first-class Value (single shared object, so
// `clock == clock`). Mirrors the VM's defineNative("clock", ...).
Value lox_clock(void);

// Global-variable write with define check: mirrors the VM's OP_SET_GLOBAL
// failure when the global is not defined yet (message + exit 70).
// Assignments never define; only declarations set the flag.
Value lox_setglobal(Value* slot, bool* defined, const char* name, int line,
                    Value rhs);

// Undefined-variable abort: mirrors the VM's OP_GET/SET_GLOBAL failure
// (message + exit 70). Lets generated code defer the check to runtime,
// exactly like the interpreter, instead of failing compilation.
Value lox_undefined(const char* name, int line);

#endif
