#include "parser.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include <arena.h>

#include "common.h"
#include "debug.h"
#include "object.h"
#include "scanner.h"

static Parser parser;

// Bump-allocated AST storage. Set for the duration of parseProgram().
static Arena* ast_arena = NULL;

// ParseError unwinding: one jmp_buf per active declaration() frame.
#define MAX_DECL_DEPTH 64
static jmp_buf decl_jmp_stack[MAX_DECL_DEPTH];
static int decl_jmp_depth = 0;

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

static void error(Token token, const char* message) {
  errorAt(&token, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

// Report + unwind to the innermost declaration(). For fatal syntax errors.
static void throwAt(Token token, const char* message) {
  errorAt(&token, message);
  if (decl_jmp_depth > 0) {
    longjmp(decl_jmp_stack[decl_jmp_depth - 1], 1);
  }
}

static void throwAtCurrent(const char* message) {
  throwAt(parser.current, message);
}

static Token peek(void) {
  return parser.current;
}

static Token previous(void) {
  return parser.previous;
}

static bool isAtEnd(void) {
  return parser.current.type == TOKEN_EOF;
}

static bool check(TokenType type) {
  if (isAtEnd()) return false;
  return peek().type == type;
}

static void advance(void) {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (!check(TOKEN_ERROR)) break;

    errorAtCurrent(parser.current.start);
  }
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

// Fatal: throws to declaration() on failure.
static Token consume(TokenType type, const char* message) {
  if (check(type)) {
    advance();
    return previous();
  }

  throwAtCurrent(message);
  // Unreachable, but appease compilers when longjmp is disabled.
  Token dummy = {0};
  return dummy;
}

static void synchronize(void) {
  advance();

  while (!isAtEnd()) {
    if (previous().type == TOKEN_SEMICOLON) break;

    switch (peek().type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        parser.panicMode = false;
        return;
      default:
        break;
    }

    advance();
  }

  parser.panicMode = false;
}

#ifndef _CLANGD

static Expr* newExpr(Expr value) {
  return New(ast_arena, Expr, 1, &value);
}

static Stmt* newStmt(Stmt value) {
  return New(ast_arena, Stmt, 1, &value);
}

void printExpr(Expr* expr) {
  if (expr == NULL) {
    printf("<error>");
    return;
  }
  match(*expr) {
    of(Literal, value) printValue(*value);
    of(Assign, name, value) {
      printf("(assign %.*s ", name->length, name->start);
      printExpr(*value);
      printf(")");
    }
    of(Binary, left, op, right) {
      printf("(%.*s ", op->length, op->start);
      printExpr(*left);
      printf(" ");
      printExpr(*right);
      printf(")");
    }
    of(Call, callee, paren, args) {
      (void)paren;
      ExprList* callArgs = *args;
      printf("(call ");
      printExpr(*callee);
      for (isize i = 0; i < callArgs->len; i++) {
        printf(" ");
        printExpr(callArgs->data[i]);
      }
      printf(")");
    }
    of(Get, object, name) {
      printf("(get ");
      printExpr(*object);
      printf(" %.*s)", name->length, name->start);
    }
    of(Grouping, inner) {
      printf("(group ");
      printExpr(*inner);
      printf(")");
    }
    of(Logical, left, op, right) {
      printf("(%.*s ", op->length, op->start);
      printExpr(*left);
      printf(" ");
      printExpr(*right);
      printf(")");
    }
    of(Set, object, name, value) {
      printf("(set ");
      printExpr(*object);
      printf(" %.*s ", name->length, name->start);
      printExpr(*value);
      printf(")");
    }
    of(Super, keyword, method) {
      (void)keyword;
      printf("(super %.*s)", method->length, method->start);
    }
    of(This, keyword) {
      (void)keyword;
      printf("this");
    }
    of(Unary, op, right) {
      printf("(%.*s ", op->length, op->start);
      printExpr(*right);
      printf(")");
    }
    of(Variable, name) printf("%.*s", name->length, name->start);
  }
}

void printStmt(Stmt* stmt) {
  if (stmt == NULL) {
    printf("<error stmt>\n");
    return;
  }
  match(*stmt) {
    of(Block, statements) {
      StmtList* body = *statements;
      printf("(block");
      for (isize i = 0; i < body->len; i++) {
        printf(" ");
        printStmt(body->data[i]);
      }
      printf(")");
    }
    of(Class, name, superclass, methods) {
      StmtList* methodList = *methods;
      printf("(class %.*s", name->length, name->start);
      if (*superclass != NULL) {
        printf(" < ");
        printExpr(*superclass);
      }
      for (isize i = 0; i < methodList->len; i++) {
        printf(" ");
        printStmt(methodList->data[i]);
      }
      printf(")");
    }
    of(Expression, expression) {
      printf("(expr ");
      printExpr(*expression);
      printf(")");
    }
    of(Function, name, params, body) {
      TokenList* paramList = *params;
      StmtList* fnBody = *body;
      printf("(fun %.*s(", name->length, name->start);
      for (isize i = 0; i < paramList->len; i++) {
        if (i > 0) printf(" ");
        printf("%.*s", paramList->data[i].length, paramList->data[i].start);
      }
      printf(")");
      for (isize i = 0; i < fnBody->len; i++) {
        printf(" ");
        printStmt(fnBody->data[i]);
      }
      printf(")");
    }
    of(If, condition, thenBranch, elseBranch) {
      printf("(if ");
      printExpr(*condition);
      printf(" ");
      printStmt(*thenBranch);
      if (*elseBranch != NULL) {
        printf(" ");
        printStmt(*elseBranch);
      }
      printf(")");
    }
    of(Print, expression) {
      printf("(print ");
      printExpr(*expression);
      printf(")");
    }
    of(Return, keyword, value) {
      (void)keyword;
      printf("(return");
      if (*value != NULL) {
        printf(" ");
        printExpr(*value);
      }
      printf(")");
    }
    of(Var, name, initializer) {
      printf("(var %.*s", name->length, name->start);
      if (*initializer != NULL) {
        printf(" ");
        printExpr(*initializer);
      }
      printf(")");
    }
    of(While, condition, body) {
      printf("(while ");
      printExpr(*condition);
      printf(" ");
      printStmt(*body);
      printf(")");
    }
  }
}

void printProgram(StmtList* statements) {
  for (isize i = 0; i < statements->len; i++) {
    printStmt(statements->data[i]);
    printf("\n");
  }
}

// Forward declarations (appendix-i order).
static Stmt* declaration(void);
static Stmt* classDeclaration(void);
static Stmt* function(const char* kind);
static Stmt* varDeclaration(void);
static Stmt* statement(void);
static Stmt* expressionStatement(void);
static Stmt* printStatement(void);
static Stmt* returnStatement(void);
static Stmt* ifStatement(void);
static Stmt* whileStatement(void);
static Stmt* forStatement(void);
static void block(StmtList* out);
static Expr* expression(void);
static Expr* assignment(void);
static Expr* logic_or(void);
static Expr* logic_and(void);
static Expr* equality(void);
static Expr* comparison(void);
static Expr* term(void);
static Expr* factor(void);
static Expr* unary(void);
static Expr* finishCall(Expr* callee);
static Expr* call(void);
static Expr* primary(void);

ParseResult parseProgram(Arena* arena, const char* source, StmtList* statements) {
  ast_arena = arena;
  parser.hadError = false;
  parser.panicMode = false;
  decl_jmp_depth = 0;

  initScanner(source);
  advance();

  while (!isAtEnd()) {
    Stmt* decl = declaration();
    if (decl != NULL) {
      *Push(arena, statements) = decl;
    }
  }

  ast_arena = NULL;
  return (ParseResult){parser.hadError};
}

void parse(const char* source) {
  byte* buf = malloc(MB(8));
  if (buf == NULL) {
    fprintf(stderr, "Out of memory in parser.\n");
    return;
  }
  Arena arena = arena_init(buf, MB(8));

  jmp_buf oom;
  if (ArenaOOM(&arena, oom)) {
    fprintf(stderr, "Out of memory in parser arena.\n");
    free(buf);
    ast_arena = NULL;
    return;
  }

  StmtList* statements = New(&arena, StmtList);
  ParseResult result = parseProgram(&arena, source, statements);
  if (!result.hadError) {
    printProgram(statements);
  }

  free(buf);
}

// declaration → classDecl | funDecl | varDecl | statement ;
static Stmt* declaration(void) {
  if (decl_jmp_depth >= MAX_DECL_DEPTH) {
    errorAtCurrent("Declaration nesting too deep.");
    synchronize();
    return NULL;
  }
  jmp_buf* slot = &decl_jmp_stack[decl_jmp_depth++];
  Stmt* result = NULL;
  if (setjmp(*slot) == 0) {
    if (match1(TOKEN_CLASS)) {
      result = classDeclaration();
    } else if (match1(TOKEN_FUN)) {
      result = function("function");
    } else if (match1(TOKEN_VAR)) {
      result = varDeclaration();
    } else {
      result = statement();
    }
  } else {
    // ParseError: unwind from a nested consume()/primary().
    synchronize();
    result = NULL;
  }
  decl_jmp_depth--;
  return result;
}

// classDecl → "class" IDENTIFIER ( "<" IDENTIFIER )? "{" function* "}" ;
static Stmt* classDeclaration(void) {
  Token name = consume(TOKEN_IDENTIFIER, "Expect class name.");

  Expr* superclass = NULL;
  if (match1(TOKEN_LESS)) {
    Token superName = consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    superclass = newExpr(Variable(superName));
  }

  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");

  StmtList* methods = New(ast_arena, StmtList, 1);
  while (!check(TOKEN_RIGHT_BRACE) && !isAtEnd()) {
    *Push(ast_arena, methods) = function("method");
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

  return newStmt(Class(name, superclass, methods));
}

// function → IDENTIFIER "(" parameters? ")" block ;
// parameters → IDENTIFIER ( "," IDENTIFIER )* ;
static Stmt* function(const char* kind) {
  char msg[64];

  snprintf(msg, sizeof(msg), "Expect %s name.", kind);
  Token name = consume(TOKEN_IDENTIFIER, msg);

  snprintf(msg, sizeof(msg), "Expect '(' after %s name.", kind);
  consume(TOKEN_LEFT_PAREN, msg);

  TokenList* params = New(ast_arena, TokenList, 1);
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      if (params->len >= 255) {
        // Non-fatal like jlox: report, keep parsing.
        error(peek(), "Can't have more than 255 parameters.");
      }
      *Push(ast_arena, params) = consume(TOKEN_IDENTIFIER, "Expect parameter name.");
    } while (match1(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

  snprintf(msg, sizeof(msg), "Expect '{' before %s body.", kind);
  consume(TOKEN_LEFT_BRACE, msg);
  StmtList* body = New(ast_arena, StmtList, 1);
  block(body);

  return newStmt(Function(name, params, body));
}

// varDecl → "var" IDENTIFIER ( "=" expression )? ";" ;
static Stmt* varDeclaration(void) {
  Token name = consume(TOKEN_IDENTIFIER, "Expect variable name.");

  Expr* initializer = NULL;
  if (match1(TOKEN_EQUAL)) {
    initializer = expression();
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  return newStmt(Var(name, initializer));
}

// statement → exprStmt | forStmt | ifStmt | printStmt | returnStmt
//           | whileStmt | block ;
static Stmt* statement(void) {
  if (match1(TOKEN_FOR)) return forStatement();
  if (match1(TOKEN_IF)) return ifStatement();
  if (match1(TOKEN_PRINT)) return printStatement();
  if (match1(TOKEN_RETURN)) return returnStatement();
  if (match1(TOKEN_WHILE)) return whileStatement();
  if (match1(TOKEN_LEFT_BRACE)) {
    StmtList* stmts = New(ast_arena, StmtList, 1);
    block(stmts);
    return newStmt(Block(stmts));
  }

  return expressionStatement();
}

// exprStmt → expression ";" ;
static Stmt* expressionStatement(void) {
  Expr* expr = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  return newStmt(Expression(expr));
}

// printStmt → "print" expression ";" ;
static Stmt* printStatement(void) {
  Expr* value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  return newStmt(Print(value));
}

// returnStmt → "return" expression? ";" ;
static Stmt* returnStatement(void) {
  Token keyword = previous();
  Expr* value = NULL;
  if (!check(TOKEN_SEMICOLON)) {
    value = expression();
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
  return newStmt(Return(keyword, value));
}

// ifStmt → "if" "(" expression ")" statement ( "else" statement )? ;
static Stmt* ifStatement(void) {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  Expr* condition = expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");

  Stmt* thenBranch = statement();
  Stmt* elseBranch = NULL;
  if (match1(TOKEN_ELSE)) {
    elseBranch = statement();
  }

  return newStmt(If(condition, thenBranch, elseBranch));
}

// whileStmt → "while" "(" expression ")" statement ;
static Stmt* whileStatement(void) {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  Expr* condition = expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
  Stmt* body = statement();

  return newStmt(While(condition, body));
}

// forStmt → "for" "(" ( varDecl | exprStmt | ";" )
//                     expression? ";" expression? ")" statement ;
// Desugared to Block/While like jlox.
static Stmt* forStatement(void) {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  Stmt* initializer = NULL;
  if (match1(TOKEN_SEMICOLON)) {
    initializer = NULL;
  } else if (match1(TOKEN_VAR)) {
    initializer = varDeclaration();
  } else {
    initializer = expressionStatement();
  }

  Expr* condition = NULL;
  if (!check(TOKEN_SEMICOLON)) {
    condition = expression();
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  Expr* increment = NULL;
  if (!check(TOKEN_RIGHT_PAREN)) {
    increment = expression();
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

  Stmt* body = statement();

  if (increment != NULL) {
    StmtList* seq = New(ast_arena, StmtList, 1);
    *Push(ast_arena, seq) = body;
    *Push(ast_arena, seq) = newStmt(Expression(increment));
    body = newStmt(Block(seq));
  }

  if (condition == NULL) {
    condition = newExpr(Literal(BOOL_VAL(true)));
  }
  body = newStmt(While(condition, body));

  if (initializer != NULL) {
    StmtList* seq = New(ast_arena, StmtList, 1);
    *Push(ast_arena, seq) = initializer;
    *Push(ast_arena, seq) = body;
    body = newStmt(Block(seq));
  }

  return body;
}

// block → "{" declaration* "}" ;
static void block(StmtList* statements) {
  while (!check(TOKEN_RIGHT_BRACE) && !isAtEnd()) {
    Stmt* decl = declaration();
    if (decl != NULL) {
      *Push(ast_arena, statements) = decl;
    }
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

// expression → assignment ;
static Expr* expression(void) {
  return assignment();
}

// assignment → ( call "." )? IDENTIFIER "=" assignment | logic_or ;
static Expr* assignment(void) {
  Expr* expr = logic_or();

  if (match1(TOKEN_EQUAL)) {
    Token equals = previous();
    Expr* value = assignment();

    if (expr != NULL) {
      match(*expr) {
        of(Variable, name) {
          return newExpr(Assign(*name, value));
        }
        of(Get, object, name) {
          return newExpr(Set(*object, *name, value));
        }
        otherwise {
          // Non-fatal like jlox ([no-throw]).
          error(equals, "Invalid assignment target.");
        }
      }
    }
  }

  return expr;
}

// logic_or → logic_and ( "or" logic_and )* ;
static Expr* logic_or(void) {
  Expr* expr = logic_and();

  while (match1(TOKEN_OR)) {
    Token op = previous();
    Expr* right = logic_and();
    expr = newExpr(Logical(expr, op, right));
  }

  return expr;
}

// logic_and → equality ( "and" equality )* ;
static Expr* logic_and(void) {
  Expr* expr = equality();

  while (match1(TOKEN_AND)) {
    Token op = previous();
    Expr* right = equality();
    expr = newExpr(Logical(expr, op, right));
  }

  return expr;
}

// equality → comparison ( ( "!=" | "==" ) comparison )* ;
static Expr* equality(void) {
  Expr* expr = comparison();

  while (matchany((TokenType[]){TOKEN_BANG_EQUAL, TOKEN_EQUAL_EQUAL, 0})) {
    Token op = previous();
    Expr* right = comparison();
    expr = newExpr(Binary(expr, op, right));
  }

  return expr;
}

// comparison → term ( ( ">" | ">=" | "<" | "<=" ) term )* ;
static Expr* comparison(void) {
  Expr* expr = term();

  while (
    matchany((TokenType[]){TOKEN_GREATER, TOKEN_GREATER_EQUAL, TOKEN_LESS, TOKEN_LESS_EQUAL, 0})) {
    Token op = previous();
    Expr* right = term();
    expr = newExpr(Binary(expr, op, right));
  }

  return expr;
}

// term → factor ( ( "-" | "+" ) factor )* ;
static Expr* term(void) {
  Expr* expr = factor();

  while (matchany((TokenType[]){TOKEN_MINUS, TOKEN_PLUS, 0})) {
    Token op = previous();
    Expr* right = factor();
    expr = newExpr(Binary(expr, op, right));
  }

  return expr;
}

// factor → unary ( ( "/" | "*" ) unary )* ;
static Expr* factor(void) {
  Expr* expr = unary();

  while (matchany((TokenType[]){TOKEN_SLASH, TOKEN_STAR, 0})) {
    Token op = previous();
    Expr* right = unary();
    expr = newExpr(Binary(expr, op, right));
  }

  return expr;
}

// unary → ( "!" | "-" ) unary | call ;
static Expr* unary(void) {
  if (matchany((TokenType[]){TOKEN_BANG, TOKEN_MINUS, 0})) {
    Token op = previous();
    Expr* right = unary();
    return newExpr(Unary(op, right));
  }

  return call();
}

// arguments → expression ( "," expression )* ;
static Expr* finishCall(Expr* callee) {
  ExprList* arguments = New(ast_arena, ExprList, 1);
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      if (arguments->len >= 255) {
        // Non-fatal like jlox: report, keep parsing.
        error(peek(), "Can't have more than 255 arguments.");
      }
      *Push(ast_arena, arguments) = expression();
    } while (match1(TOKEN_COMMA));
  }

  Token paren = consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

  return newExpr(Call(callee, paren, arguments));
}

// call → primary ( "(" arguments? ")" | "." IDENTIFIER )* ;
static Expr* call(void) {
  Expr* expr = primary();

  for (;;) {
    if (match1(TOKEN_LEFT_PAREN)) {
      expr = finishCall(expr);
    } else if (match1(TOKEN_DOT)) {
      Token name = consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
      expr = newExpr(Get(expr, name));
    } else {
      break;
    }
  }

  return expr;
}

// primary → "true" | "false" | "nil" | "this" | NUMBER | STRING
//         | IDENTIFIER | "(" expression ")"
//         | "super" "." IDENTIFIER ;
static Expr* primary(void) {
  if (match1(TOKEN_FALSE)) return newExpr(Literal(BOOL_VAL(false)));
  if (match1(TOKEN_TRUE)) return newExpr(Literal(BOOL_VAL(true)));
  if (match1(TOKEN_NIL)) return newExpr(Literal(NIL_VAL));

  if (match1(TOKEN_NUMBER)) {
    double number = strtod(previous().start, NULL);
    return newExpr(Literal(NUMBER_VAL(number)));
  }

  if (match1(TOKEN_STRING)) {
    Token str = previous();
    return newExpr(Literal(OBJ_VAL(copyString(str.start + 1, str.length - 2))));
  }

  if (match1(TOKEN_SUPER)) {
    Token keyword = previous();
    consume(TOKEN_DOT, "Expect '.' after 'super'.");
    Token method = consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
    return newExpr(Super(keyword, method));
  }

  if (match1(TOKEN_THIS)) return newExpr(This(previous()));

  if (match1(TOKEN_IDENTIFIER)) {
    return newExpr(Variable(previous()));
  }

  if (match1(TOKEN_LEFT_PAREN)) {
    Expr* expr = expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
    return newExpr(Grouping(expr));
  }

  throwAtCurrent("Expect expression.");
  return NULL;  // Unreachable.
}

#endif
