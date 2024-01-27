#ifndef clox_parser_h
#define clox_parser_h

#include <datatype99.h>
#include <stdbool.h>

#include "scanner.h"

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
  (Literal, Token),
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

Expr* parse(const char* source);

#endif  // clox_parser_h
