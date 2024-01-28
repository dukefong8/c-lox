#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

#include "common.h"
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

Expr* expression();
Expr* assignment();
Expr* logic_or();
Expr* logic_and();
Expr* equality();
Expr* comparision();
Expr* term();
Expr* factor();
Expr* unary();
Expr* primary();

Expr* parse(const char* source) {
  initScanner(source);
  parser.hadError = false;
  parser.panicMode = false;

  advance();
  Expr* expr = expression();
  expect(TOKEN_EOF, "Expect end of expression.");
  return expr;
}

/*
  expression   → assignment ;
 */
Expr* expression() {
  return assignment();
}

/*
  assignment   → IDENTIFIER "=" assignment
               | logic_or ;
 */
Expr* assignment() {
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
Expr* logic_or() {
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
Expr* logic_and() {
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
Expr* equality() {
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
Expr* comparision() {
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
Expr* term() {
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
Expr* factor() {
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
Expr* unary() {
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
Expr* primary() {
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
