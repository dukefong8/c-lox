#ifndef clox_parser_h
#define clox_parser_h

#include <datatype99.h>
#include <list.h>
#include <stdbool.h>

#include "scanner.h"
#include "value.h"

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

void parse(const char* source);

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
  Expr* expr;
  struct list_head list;
} Expr_list;

typedef struct {
  Stmt* stmt;
  struct list_head list;
} Stmt_list;

typedef struct {
  Token token;
  struct list_head list;
} Token_list;

#ifndef _CLANGD
// clang-format off
datatype(Expr,
  (Literal, Value),
  (Variable, Token),
  (Call, Expr*, Token, Expr_list),
  (This, Token),
  (Super, Token, Token),
  (Get, Expr*, Token),
  (Set, Expr*, Token, Expr*),
  (Grouping, Expr*),
  (Assign, Token, Expr*),
  (Unary, Token, Expr*),
  (Binary, Expr*, Token, Expr*),
  (Logical, Expr*, Token, Expr*)
);

datatype(Stmt,
  (Block, Stmt_list),
  (Class, Token, ExprVariable, Stmt_list),
  (Expression, Expr*),
  (Print, Expr*),
  (Function, Token, Token_list, Stmt_list),
  (If, Stmt*, Stmt*),
  (Return, Token, Expr*),
  (Var, Token, Expr*),
  (While, Expr*, Stmt*)
);
// clang-format on

#endif  // _CLANGD

#endif  // clox_parser_h
