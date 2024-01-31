#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "debug.h"
#include "scanner.h"

static Parser parser;

static Token peek() {
  return parser.current;
}

static Token previous() {
  return parser.previous;
}

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
  breakpoint();
}

static void error(const char* message) {
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

static void advance() {
  parser.previous = parser.current;

  for (;;) {
    asm("a1:");
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}

static bool check(TokenType type) {
  return peek().type == type;
}

static bool match1(TokenType type) {
  if (check(type)) {
    advance();
    return true;
  }

  return false;
}

static bool matchany(const TokenType* types) {
  while (*types) {
    if (check(*types++)) {
      advance();
      return true;
    }
  }

  return false;
}

static void expect(TokenType type, const char* message) {
  if (check(type)) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

#define New(Type, Value)                        \
  ({                                            \
    Type* value_ = (Type*)malloc(sizeof(Type)); \
    *value_ = (Value);                          \
    value_;                                     \
  })

#ifndef _CLANGD

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

static Stmt* statement();
static Expr* expression();
static Expr* assignment();
static Expr* logic_or();
static Expr* logic_and();
static Expr* equality();
static Expr* comparision();
static Expr* term();
static Expr* factor();
static Expr* unary();
static Expr* primary();

void parse(const char* source) {
  initScanner(source);
  parser.hadError = false;
  parser.panicMode = false;

  advance();
  Expr* expr = expression();
  expect(TOKEN_EOF, "Expect end of expression.");
  printExpr(expr);
}

/*
  expression   → assignment ;
 */
static Expr* expression() {
  return assignment();
}

static Stmt* printStatement() {
  Expr* expr = expression();
  expect(TOKEN_SEMICOLON, "Expect ';' after value.");
  return New(Stmt, Print(expr));
}

static Stmt* expressionStatement() {
  Expr* expr = expression();
  expect(TOKEN_SEMICOLON, "Expect ';' after value.");
  return New(Stmt, Expression(expr));
}

static Stmt* statement() {
  if (match1(TOKEN_PRINT)) {
    return printStatement();
  }

  return expressionStatement();
}

/*
  assignment   → IDENTIFIER "=" assignment
               | logic_or ;
 */
static Expr* assignment() {
  Expr* expr = logic_or();

  if (match1(TOKEN_EQUAL)) {
    match(*expr) {
      of(Variable, name) {
        Expr* value = assignment();
        return New(Expr, Assign(*name, value));
      }
      otherwise error("Expect identifier");
    }
  }

  return expr;
}

/*
  logic_or      → logic_and ( "or" logic_and )* ;
 */
static Expr* logic_or() {
  Expr* expr = logic_and();

  while (match1(TOKEN_OR)) {
    Token op = previous();
    Expr* right = logic_and();
    expr = New(Expr, Logical(expr, op, right));
  }

  return expr;
}

/*
  logic_and      → equality ( "and" equality )* ;
 */
static Expr* logic_and() {
  Expr* expr = equality();

  while (match1(TOKEN_AND)) {
    Token op = previous();
    Expr* right = equality();
    expr = New(Expr, Logical(expr, op, right));
  }

  return expr;
}

/*
  equality      → comparison ( ( "!=" | "==" ) comparison )* ;
 */
static Expr* equality() {
  Expr* expr = comparision();

  while (matchany((TokenType[]){TOKEN_BANG_EQUAL, TOKEN_EQUAL_EQUAL, 0})) {
    Token op = previous();
    Expr* right = comparision();
    expr = New(Expr, Binary(expr, op, right));
  }

  return expr;
}

/*
  comparison     → term ( ( ">" | ">=" | "<" | "<=" ) term )* ;
 */
static Expr* comparision() {
  Expr* expr = term();

  while (
    matchany((TokenType[]){TOKEN_GREATER, TOKEN_GREATER_EQUAL, TOKEN_LESS, TOKEN_LESS_EQUAL, 0})) {
    Token op = previous();
    Expr* right = term();
    expr = New(Expr, Binary(expr, op, right));
  }

  return expr;
}

/*
  term          → factor ( ( "-" | "+" ) factor )* ;
 */
static Expr* term() {
  Expr* expr = factor();

  while (matchany((TokenType[]){TOKEN_MINUS, TOKEN_PLUS, 0})) {
    Token op = previous();
    Expr* right = factor();
    expr = New(Expr, Binary(expr, op, right));
  }

  return expr;
}

/*
 factor         → unary ( ( "/" | "*" ) unary )* ;
 */
static Expr* factor() {
  Expr* expr = unary();

  while (matchany((TokenType[]){TOKEN_SLASH, TOKEN_STAR, 0})) {
    Token op = previous();
    Expr* right = unary();
    expr = New(Expr, Binary(expr, op, right));
  }

  return expr;
}

/*
  unary        → ( "!" | "-" ) unary
               | primary ;
 */
static Expr* unary() {
  if (matchany((TokenType[]){TOKEN_BANG, TOKEN_MINUS, 0})) {
    Token op = previous();
    Expr* right = unary();
    return New(Expr, Unary(op, right));
  }

  return primary();
}

/*
  primary      → NUMBER | STRING | IDENTIFIER | "true" | "false" | "nil"
               | "(" expression ")" ;
 */
static Expr* primary() {
  if (match1(TOKEN_LEFT_PAREN)) {
    Expr* expr = expression();
    expect(TOKEN_RIGHT_PAREN, "Expect ')' after expression");
    return New(Expr, Grouping(expr));
  }

  if (match1(TOKEN_IDENTIFIER)) {
    return New(Expr, Variable(previous()));
  }

  advance();
  return New(Expr, Literal(previous()));
}

#undef New
#endif
