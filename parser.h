#ifndef clox_parser_h
#define clox_parser_h

#include <datatype99.h>
#include <stdbool.h>

#include <arena.h>

#include "scanner.h"
#include "value.h"

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

// Lists are movable `Slice` descriptors. Allocate one in the arena with
// `New(arena, XxxList)` and pass/store the resulting `XxxList*`; the parser
// writes into the caller's descriptor.
typedef Slice(Expr*) ExprList;

typedef Slice(Stmt*) StmtList;

typedef Slice(Token) TokenList;

#ifndef _CLANGD
// clang-format off
// Complete Lox grammar, appendix-i:
// https://github.com/munificent/craftinginterpreters/blob/master/book/appendix-i.md
datatype(Expr,
  (Literal, Value),
  (Assign, Token, Expr*),
  (Binary, Expr*, Token, Expr*),
  (Call, Expr*, Token, ExprList*),
  (Get, Expr*, Token),
  (Grouping, Expr*),
  (Logical, Expr*, Token, Expr*),
  (Set, Expr*, Token, Expr*),
  (Super, Token, Token),
  (This, Token),
  (Unary, Token, Expr*),
  (Variable, Token)
);

datatype(Stmt,
  (Block, StmtList*),
  (Class, Token, Expr*, StmtList*),
  (Expression, Expr*),
  (Function, Token, TokenList*, StmtList*),
  (If, Expr*, Stmt*, Stmt*),
  (Print, Expr*),
  (Return, Token, Expr*),
  (Var, Token, Expr*),
  (While, Expr*, Stmt*)
);
// clang-format on

#endif  // _CLANGD

typedef struct {
  bool hadError;
} ParseResult;

// Parse a full program (declaration* EOF) into arena-allocated AST. The
// caller supplies an arena-allocated descriptor
// (`StmtList* s = New(arena, StmtList);`); the parser writes into it.
// Arena must outlive the AST. String literal Values reference VM-heap
// ObjStrings (see copyString), not the arena.
ParseResult parseProgram(Arena* arena, const char* source, StmtList* statements);

// Debug helper: parse + pretty-print a program. Manages its own arena.
void parse(const char* source);

#ifndef _CLANGD
void printExpr(Expr* expr);
void printStmt(Stmt* stmt);
void printProgram(StmtList* statements);
#endif

#endif  // clox_parser_h
