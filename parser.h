#ifndef clox_parser_h
#define clox_parser_h

#include <datatype99.h>

#include "scanner.h"
#include "value.h"

void parse(const char* source);

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

// clang-format off
#ifndef _CLANGD

datatype(
  Expr,
  (Literal, Value),
  (Assign, Token, Expr*),
  (Unary, Token, Expr*),
  (Binary, Expr*, Token, Expr*),
  (Grouping, Expr*)
);

datatype(
  Stmt,
  (Expression, Expr*),
  (Print, Expr*)
);

#endif // _CLANGD
// clang-format on

#endif  // clox_parser_h
