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

Expr* parse(const char* source);

static void printExpr(Expr* expr) {
  match(*expr) {
    of(Literal, token) printf("%.*s", token->length, token->start);
    of(Variable, token) printf("%.*s", token->length, token->start);
    of(Grouping, expr) printExpr(*expr);
    of(Assign, var, expr) {
      printf("%.*s = ", var->length, var->start);
      printExpr(*expr);
    }
    of(Unary, op, expr) {
      printf("%.*s", op->length, op->start);
      printExpr(*expr);
    }
    of(Binary, left, op, right) {
      printf("(");
      printf("%.*s ", op->length, op->start);
      printExpr(*left);
      printf(" ");
      printExpr(*right);
      printf(")");
    }
    of(Logical, left, op, right) {
      printf("(");
      printf(" %.*s ", op->length, op->start);
      printExpr(*left);
      printf(" ");
      printExpr(*right);
      printf(")");
    }
  }
}

#endif  // _CLANGD
#endif  // clox_parser_h
