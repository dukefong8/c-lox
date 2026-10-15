#include "loxrt.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "memory.h"
#include "object.h"
#include "vm.h"

void lox_init(void) {
  initVM();
  // Generated C keeps every live Value in C locals, globals and malloc'd
  // cells — none of which the mark-sweep collector traces. Collecting
  // mid-run could free reachable strings, so the cgen path never collects;
  // everything is still freed by lox_free at exit.
  vm.nextGC = SIZE_MAX;
}

void lox_free(void) {
  freeVM();
}

Value lox_string(const char* chars, int length) {
  return OBJ_VAL(copyString(chars, length));
}

bool lox_is_falsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void lox_error(int line, const char* message) {
  fprintf(stderr, "%s\n[line %d] in script\n", message, line);
  exit(70);
}

static void require_numbers(Value a, Value b, int line) {
  if (!IS_NUMBER(a) || !IS_NUMBER(b)) lox_error(line, "Operands must be numbers.");
}

Value lox_add(Value a, Value b, int line) {
  if (IS_STRING(a) && IS_STRING(b)) {
    ObjString* bs = AS_STRING(b);
    ObjString* as = AS_STRING(a);
    int length = as->length + bs->length;
    char* chars = ALLOCATE(char, length + 1);
    memcpy(chars, as->chars, as->length);
    memcpy(chars + as->length, bs->chars, bs->length);
    chars[length] = '\0';
    return OBJ_VAL(takeString(chars, length));
  }
  if (IS_NUMBER(a) && IS_NUMBER(b)) return NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
  lox_error(line, "Operands must be two numbers or two strings.");
  return NIL_VAL; /* unreachable */
}

Value lox_sub(Value a, Value b, int line) {
  require_numbers(a, b, line);
  return NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
}

Value lox_mul(Value a, Value b, int line) {
  require_numbers(a, b, line);
  return NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
}

Value lox_div(Value a, Value b, int line) {
  require_numbers(a, b, line);
  return NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b));
}

Value lox_negate(Value value, int line) {
  if (!IS_NUMBER(value)) lox_error(line, "Operand must be a number.");
  return NUMBER_VAL(-AS_NUMBER(value));
}

Value lox_not(Value value) {
  return BOOL_VAL(lox_is_falsey(value));
}

Value lox_equal(Value a, Value b) {
  return BOOL_VAL(valuesEqual(a, b));
}

Value lox_greater(Value a, Value b, int line) {
  require_numbers(a, b, line);
  return BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b));
}

Value lox_less(Value a, Value b, int line) {
  require_numbers(a, b, line);
  return BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b));
}

void lox_print(Value value) {
  printValue(value);
  printf("\n");
}

Value lox_undefined(const char* name, int line) {
  fprintf(stderr, "Undefined variable '%s'.\n[line %d] in script\n", name, line);
  exit(70);
  return NIL_VAL; /* unreachable */
}

LoxCell* lox_newcell(Value value) {
  LoxCell* cell = (LoxCell*)malloc(sizeof(LoxCell));
  if (cell == NULL) exit(1);
  cell->value = value;
  return cell;
}

void lox_arity(int line, int want, int got) {
  char message[64];
  snprintf(message, sizeof(message), "Expected %d arguments but got %d.", want,
           got);
  lox_error(line, message);
}

static Value clockNative(int argCount, Value* args) {
  (void)argCount;
  (void)args;
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

Value lox_clock(void) {
  static Value cached;
  static bool initialized = false;
  if (!initialized) {
    cached = OBJ_VAL(newNative(clockNative));
    initialized = true;
  }
  return cached;
}

Value lox_call(Value callee, Value* args, int argCount, int line) {
  if (IS_CFUN(callee)) {
    ObjCFun* function = AS_CFUN(callee);
    if (argCount != function->arity) {
      lox_arity(line, function->arity, argCount);
    }
    return function->function(function, args, argCount, line);
  }
  if (IS_NATIVE(callee)) {
    return AS_NATIVE(callee)(argCount, args);
  }
  lox_error(line, "Can only call functions and classes.");
  return NIL_VAL; /* unreachable */
}

Value lox_setglobal(Value* slot, bool* defined, const char* name, int line,
                    Value rhs) {
  if (!*defined) lox_undefined(name, line);
  *slot = rhs;
  return rhs;
}
