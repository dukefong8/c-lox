#ifndef clox_parser_h
#define clox_parser_h

#include <datatype99.h>
#include <stdbool.h>
#include <stdio.h>

#include "scanner.h"

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

#ifndef _CLANGD

// clang-format off
datatype(Expr,
  (Literal, Token),
  (Variable, Token),
  (Grouping, Expr*),
  (Assign, Token, Expr*),
  (Unary, Token, Expr*),
  (Binary, Expr*, Token, Expr*),
  (Logical, Expr*, Token, Expr*)
);

datatype(Stmt,
  (Expression, Expr*),
  (Print, Expr*)
);
// clang-format on

void parse(const char* source);

#endif  // _CLANGD
#endif  // clox_parser_h
