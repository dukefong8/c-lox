#include "cgen.h"
#include <stdio.h>
#include <string.h>
#include "debug.h"
#include "object.h"
#include "value.h"

#ifndef _CLANGD

typedef Slice(int) IntList;

typedef Slice(bool) BoolList;

typedef struct {
  int funcId;
  IntList* uids;
} CgenEnv;

typedef Slice(CgenEnv) CgenEnvList;

typedef struct {
  Arena* arena;
  int uid;
  bool hadError;
  int funcCount;
  // Per-uid tables (index uid-1): declaring function, script-direct global,
  // captured-by-nested-function, source token (for names).
  IntList* uidFunc;
  BoolList* uidGlobal;
  BoolList* uidCaptured;
  TokenList* uidToken;
  // Variable-use records for capture analysis: (uid, using function).
  IntList* useUid;
  IntList* useFunc;
  // Function nesting: parent funcId per fid (index fid-1), script is 0.
  IntList* funcParent;
  int clockUid;
  // Emit state: environments and the function being emitted (0 = script).
  CgenEnvList* envs;
  int emitFunc;
} CGen;

static void cgen_error(CGen* g, int line, const char* message) {
  fprintf(stderr, "[line %d] Error: %s\n", line, message);
  g->hadError = true;
}

// Allocate a fresh uid, recording its declaring function (-1 unknown, used
// for lowered temps that never cross function boundaries).
static int new_uid(CGen* g, int funcId, Token token, bool isGlobal) {
  int uid = ++g->uid;
  *Push(g->arena, g->uidFunc) = funcId;
  *Push(g->arena, g->uidGlobal) = isGlobal;
  *Push(g->arena, g->uidCaptured) = (bool)false;
  *Push(g->arena, g->uidToken) = token;
  return uid;
}

static int uid_func(CGen* g, int uid) {
  if (uid <= 0 || uid > (int)g->uidFunc->len)
    return -1;
  return g->uidFunc->data[uid - 1];
}

static bool uid_is_global(CGen* g, int uid) {
  if (uid <= 0 || uid > (int)g->uidGlobal->len)
    return false;
  return g->uidGlobal->data[uid - 1];
}

static bool uid_is_boxed(CGen* g, int uid) {
  if (uid <= 0 || uid > (int)g->uidCaptured->len)
    return false;
  return g->uidCaptured->data[uid - 1];
}

// ---- L_res / L_flat IR: resolved core AST ----

typedef struct RExpr RExpr;
typedef struct RStmt RStmt;

typedef Slice(RExpr*) RExprList;

typedef Slice(RStmt*) RStmtList;

// clang-format off
datatype(RExpr,
  (RLiteral, Value),
  (RVar, Token, int),
  (RAssign, Token, int, RExpr*),
  (RBinary, RExpr*, Token, RExpr*),
  (RGrouping, RExpr*),
  (RLogical, RExpr*, Token, RExpr*),
  (RUnary, Token, RExpr*),
  (RCall, RExpr*, Token, RExprList*)
);

typedef struct {
  Token name;
  int uid;
} CgenParam;

typedef Slice(CgenParam) CgenParamList;

datatype(RStmt,
  (RBlock, RStmtList*),
  (RExpression, RExpr*),
  (RFunDecl, Token, int, int, CgenParamList*, RStmtList*),
  (RIf, RExpr*, RStmt*, RStmt*),
  (RPrint, RExpr*),
  (RReturn, Token, RExpr*),
  (RVarDecl, Token, int, RExpr*),
  (RWhile, RExpr*, RStmt*, RStmtList*)
);
// clang-format on

static RExpr* newRExpr(CGen* g, RExpr value) {
  return New(g->arena, RExpr, 1, &value);
}

static RStmt* newRStmt(CGen* g, RStmt value) {
  return New(g->arena, RStmt, 1, &value);
}

// ---- Pass 1: check-core (L_src -> L_core or error) ----

static void check_expr(CGen* g, Expr* e) {
  if (g->hadError)
    return;
  match(*e) {
    of(Literal, value) {
      (void)value;
    }
    of(Assign, name, val) {
      (void)name;
      check_expr(g, *val);
    }
    of(Binary, left, op, right) {
      (void)op;
      check_expr(g, *left);
      check_expr(g, *right);
    }
    of(Call, callee, paren, args) {
      (void)paren;
      ExprList* callArgs = *args;
      check_expr(g, *callee);
      for (isize i = 0; i < callArgs->len; i++)
        check_expr(g, callArgs->data[i]);
    }
    of(Get, object, name) {
      (void)object;
      cgen_error(g, name->line, "unsupported 'properties'");
    }
    of(Grouping, inner) {
      check_expr(g, *inner);
    }
    of(Logical, left, op, right) {
      (void)op;
      check_expr(g, *left);
      check_expr(g, *right);
    }
    of(Set, object, name, value) {
      (void)object;
      (void)value;
      cgen_error(g, name->line, "unsupported 'setters'");
    }
    of(Super, keyword, method) {
      (void)method;
      cgen_error(g, keyword->line, "unsupported 'super'");
    }
    of(This, keyword) {
      cgen_error(g, keyword->line, "unsupported 'this'");
    }
    of(Unary, op, right) {
      (void)op;
      check_expr(g, *right);
    }
    of(Variable, name) {
      (void)name;
    }
  }
}

static void check_stmt(CGen* g, Stmt* s, bool inFun) {
  if (g->hadError)
    return;
  match(*s) {
    of(Block, statements) {
      StmtList* body = *statements;
      for (isize i = 0; i < body->len; i++)
        check_stmt(g, body->data[i], inFun);
    }
    of(Class, name, superclass, methods) {
      (void)superclass;
      (void)methods;
      cgen_error(g, name->line, "unsupported 'classes'");
    }
    of(Expression, expression) {
      check_expr(g, *expression);
    }
    of(Function, name, params, body) {
      (void)name;
      (void)params;
      StmtList* fnBody = *body;
      for (isize i = 0; i < fnBody->len; i++)
        check_stmt(g, fnBody->data[i], true);
    }
    of(If, condition, thenBranch, elseBranch) {
      check_expr(g, *condition);
      check_stmt(g, *thenBranch, inFun);
      if (*elseBranch != NULL)
        check_stmt(g, *elseBranch, inFun);
    }
    of(Print, expression) {
      check_expr(g, *expression);
    }
    of(Return, keyword, value) {
      if (!inFun) {
        cgen_error(g, keyword->line, "Can't return from top-level code.");
        return;
      }
      if (*value != NULL)
        check_expr(g, *value);
    }
    of(Var, name, initializer) {
      (void)name;
      if (*initializer != NULL)
        check_expr(g, *initializer);
    }
    of(While, condition, body) {
      check_expr(g, *condition);
      check_stmt(g, *body, inFun);
    }
  }
}

// ---- Pass 2: resolve-scopes (L_core -> L_res) ----

typedef struct {
  const char* start;
  int length;
  int uid;
} CgenBinding;

typedef Slice(CgenBinding) CgenBindingList;

typedef struct CgenScope {
  CgenBindingList* bindings;
  struct CgenScope* parent;
  int funcId;
  bool isScriptDirect;
} CgenScope;

static CgenScope* new_scope(CGen* g, CgenScope* parent, int funcId) {
  CgenScope* scope = New(g->arena, CgenScope, 1);
  scope->bindings = New(g->arena, CgenBindingList, 1);
  scope->parent = parent;
  scope->funcId = funcId;
  scope->isScriptDirect = false;
  return scope;
}

// Bind a name in this scope, allocating a uid. A duplicate in the same
// LOCAL scope is a compile error (mirror declareVariable); the script scope
// allows redefinition (global semantics). Returns the uid, or -1 on error.
static int scope_bind(CGen* g, CgenScope* scope, Token name) {
  if (!scope->isScriptDirect) {
    for (isize i = 0; i < scope->bindings->len; i++) {
      CgenBinding* b = &scope->bindings->data[i];
      if (b->length == name.length && memcmp(b->start, name.start, (size_t)name.length) == 0) {
        cgen_error(g, name.line, "Already a variable with this name in this scope.");
        return -1;
      }
    }
  }
  int uid = new_uid(g, scope->funcId, name, scope->isScriptDirect);
  CgenBinding binding = {name.start, name.length, uid};
  *Push(g->arena, scope->bindings) = binding;
  return uid;
}

// Reuse the uid when this exact declaration node was pre-bound (top-level
// function hoisting); otherwise bind fresh.
static int scope_reuse_or_bind(CGen* g, CgenScope* scope, Token name) {
  for (isize i = 0; i < scope->bindings->len; i++) {
    CgenBinding* b = &scope->bindings->data[i];
    if (b->start == name.start && b->length == name.length)
      return b->uid;
  }
  return scope_bind(g, scope, name);
}

static bool scope_lookup(CGen* g, CgenScope* scope, Token name, int* outUid, int* outFunc) {
  (void)g;
  for (CgenScope* s = scope; s != NULL; s = s->parent) {
    // Backwards: a later declaration in the same scope shadows earlier ones,
    // matching global redefinition semantics.
    for (isize i = s->bindings->len - 1; i >= 0; i--) {
      CgenBinding* b = &s->bindings->data[i];
      if (b->length == name.length && memcmp(b->start, name.start, (size_t)name.length) == 0) {
        *outUid = b->uid;
        *outFunc = s->funcId;
        return true;
      }
    }
  }
  return false;
}

// Record a use of uid from function userFunc for capture analysis: a
// non-global uid used outside its declaring function must live in a heap
// cell shared with nested closures.
static void record_use(CGen* g, int uid, int declFunc, int userFunc) {
  *Push(g->arena, g->useUid) = uid;
  *Push(g->arena, g->useFunc) = userFunc;
  if (uid > 0 && declFunc != userFunc && !uid_is_global(g, uid)) {
    g->uidCaptured->data[uid - 1] = true;
  }
}

static RExpr* resolve_expr(CGen* g, CgenScope* scope, Expr* e);
static RStmt* resolve_stmt(CGen* g, CgenScope* scope, Stmt* s);
static void resolve_list(CGen* g, CgenScope* scope, StmtList* stmts, RStmtList* out);

static RExpr* resolve_expr(CGen* g, CgenScope* scope, Expr* e) {
  if (g->hadError)
    return NULL;
  match(*e) {
    of(Literal, value) {
      return newRExpr(g, RLiteral(*value));
    }
    of(Assign, name, val) {
      // Unresolvable targets lower to a runtime abort (parity with the
      // VM's OP_SET_GLOBAL failure); the value still evaluates first.
      int uid = -1, declFunc = -1;
      scope_lookup(g, scope, *name, &uid, &declFunc);
      RExpr* value = resolve_expr(g, scope, *val);
      if (g->hadError)
        return NULL;
      if (uid >= 0)
        record_use(g, uid, declFunc, scope->funcId);
      return newRExpr(g, RAssign(*name, uid, value));
    }
    of(Binary, left, op, right) {
      RExpr* l = resolve_expr(g, scope, *left);
      if (g->hadError)
        return NULL;
      RExpr* r = resolve_expr(g, scope, *right);
      if (g->hadError)
        return NULL;
      return newRExpr(g, RBinary(l, *op, r));
    }
    of(Call, callee, paren, args) {
      RExpr* c = resolve_expr(g, scope, *callee);
      if (g->hadError)
        return NULL;
      ExprList* callArgs = *args;
      RExprList* rargs = New(g->arena, RExprList, 1);
      for (isize i = 0; i < callArgs->len; i++) {
        RExpr* a = resolve_expr(g, scope, callArgs->data[i]);
        if (g->hadError)
          return NULL;
        *Push(g->arena, rargs) = a;
      }
      return newRExpr(g, RCall(c, *paren, rargs));
    }
    of(Get, object, name) {
      (void)object;
      cgen_error(g, name->line, "internal: unlowered get");
      return NULL;
    }
    of(Grouping, inner) {
      RExpr* x = resolve_expr(g, scope, *inner);
      if (g->hadError)
        return NULL;
      return newRExpr(g, RGrouping(x));
    }
    of(Logical, left, op, right) {
      RExpr* l = resolve_expr(g, scope, *left);
      if (g->hadError)
        return NULL;
      RExpr* r = resolve_expr(g, scope, *right);
      if (g->hadError)
        return NULL;
      return newRExpr(g, RLogical(l, *op, r));
    }
    of(Set, object, name, value) {
      (void)object;
      (void)value;
      cgen_error(g, name->line, "internal: unlowered set");
      return NULL;
    }
    of(Super, keyword, method) {
      (void)method;
      cgen_error(g, keyword->line, "internal: unlowered super");
      return NULL;
    }
    of(This, keyword) {
      cgen_error(g, keyword->line, "internal: unlowered this");
      return NULL;
    }
    of(Unary, op, right) {
      RExpr* x = resolve_expr(g, scope, *right);
      if (g->hadError)
        return NULL;
      return newRExpr(g, RUnary(*op, x));
    }
    of(Variable, name) {
      // Unresolvable names lower to a runtime abort, matching the VM's
      // OP_GET_GLOBAL failure (message + exit 70), even when unreachable.
      int uid = -1, declFunc = -1;
      scope_lookup(g, scope, *name, &uid, &declFunc);
      if (uid >= 0)
        record_use(g, uid, declFunc, scope->funcId);
      return newRExpr(g, RVar(*name, uid));
    }
  }
  return NULL; /* unreachable */
}

static RStmt* resolve_branch(CGen* g, CgenScope* scope, Stmt* s) {
  return resolve_stmt(g, new_scope(g, scope, scope->funcId), s);
}

// Scan a resolved initializer for a use of the variable being declared
// (the VM errors on reading a local in its own initializer).
static bool init_uses_uid(CGen* g, RExpr* e, int uid) {
  (void)g;
  match(*e) {
    of(RLiteral, value) {
      (void)value;
      return false;
    }
    of(RVar, token, use) {
      (void)token;
      return *use == uid;
    }
    of(RAssign, token, use, value) {
      (void)token;
      if (*use == uid)
        return true;
      return init_uses_uid(g, *value, uid);
    }
    of(RBinary, left, op, right) {
      (void)op;
      return init_uses_uid(g, *left, uid) || init_uses_uid(g, *right, uid);
    }
    of(RGrouping, inner) {
      return init_uses_uid(g, *inner, uid);
    }
    of(RLogical, left, op, right) {
      (void)op;
      return init_uses_uid(g, *left, uid) || init_uses_uid(g, *right, uid);
    }
    of(RUnary, op, right) {
      (void)op;
      return init_uses_uid(g, *right, uid);
    }
    of(RCall, callee, paren, args) {
      (void)paren;
      RExprList* callArgs = *args;
      if (init_uses_uid(g, *callee, uid))
        return true;
      for (isize i = 0; i < callArgs->len; i++) {
        if (init_uses_uid(g, callArgs->data[i], uid))
          return true;
      }
      return false;
    }
  }
  return false;
}

static RStmt* resolve_stmt(CGen* g, CgenScope* scope, Stmt* s) {
  if (g->hadError)
    return NULL;
  match(*s) {
    of(Block, statements) {
      RStmtList* body = New(g->arena, RStmtList, 1);
      resolve_list(g, new_scope(g, scope, scope->funcId), *statements, body);
      if (g->hadError)
        return NULL;
      return newRStmt(g, RBlock(body));
    }
    of(Class, name, superclass, methods) {
      (void)superclass;
      (void)methods;
      cgen_error(g, name->line, "internal: unlowered class");
      return NULL;
    }
    of(Expression, expression) {
      RExpr* x = resolve_expr(g, scope, *expression);
      if (g->hadError)
        return NULL;
      return newRStmt(g, RExpression(x));
    }
    of(Function, name, params, body) {
      // Bound in the enclosing scope before resolving the body so the
      // function can recurse. Top-level names may already be pre-bound.
      int uid = scope_reuse_or_bind(g, scope, *name);
      if (g->hadError)
        return NULL;
      int fid = ++g->funcCount;
      *Push(g->arena, g->funcParent) = scope->funcId;
      CgenScope* fscope = new_scope(g, scope, fid);
      TokenList* paramList = *params;
      CgenParamList* rparams = New(g->arena, CgenParamList, 1);
      for (isize i = 0; i < paramList->len; i++) {
        int puid = scope_bind(g, fscope, paramList->data[i]);
        if (g->hadError)
          return NULL;
        CgenParam p = {paramList->data[i], puid};
        *Push(g->arena, rparams) = p;
      }
      RStmtList* rbody = New(g->arena, RStmtList, 1);
      resolve_list(g, fscope, *body, rbody);
      if (g->hadError)
        return NULL;
      return newRStmt(g, RFunDecl(*name, uid, fid, rparams, rbody));
    }
    of(If, condition, thenBranch, elseBranch) {
      RExpr* cond = resolve_expr(g, scope, *condition);
      if (g->hadError)
        return NULL;
      RStmt* thenB = resolve_branch(g, scope, *thenBranch);
      if (g->hadError)
        return NULL;
      RStmt* elseB = NULL;
      if (*elseBranch != NULL) {
        elseB = resolve_branch(g, scope, *elseBranch);
        if (g->hadError)
          return NULL;
      }
      return newRStmt(g, RIf(cond, thenB, elseB));
    }
    of(Print, expression) {
      RExpr* x = resolve_expr(g, scope, *expression);
      if (g->hadError)
        return NULL;
      return newRStmt(g, RPrint(x));
    }
    of(Return, keyword, value) {
      RExpr* v = NULL;
      if (*value != NULL) {
        v = resolve_expr(g, scope, *value);
        if (g->hadError)
          return NULL;
      }
      return newRStmt(g, RReturn(*keyword, v));
    }
    of(Var, name, initializer) {
      if (scope->isScriptDirect) {
        // Initializer resolves in the ENCLOSING scope: it cannot see its
        // own var (script scope has no locals; a same-name global read is
        // legal and resolves to the earlier binding).
        RExpr* init = NULL;
        if (*initializer != NULL) {
          init = resolve_expr(g, scope, *initializer);
          if (g->hadError)
            return NULL;
        }
        int uid = scope_bind(g, scope, *name);
        if (g->hadError)
          return NULL;
        return newRStmt(g, RVarDecl(*name, uid, init));
      }
      // Local scope: bind first (mirror declareVariable), so the
      // initializer sees the variable itself as uninitialized...
      int uid = scope_bind(g, scope, *name);
      if (g->hadError)
        return NULL;
      RExpr* init = NULL;
      if (*initializer != NULL) {
        init = resolve_expr(g, scope, *initializer);
        if (g->hadError)
          return NULL;
        if (init_uses_uid(g, init, uid)) {
          cgen_error(g, name->line, "Can't read local variable in its own initializer.");
          return NULL;
        }
      }
      return newRStmt(g, RVarDecl(*name, uid, init));
    }
    of(While, condition, body) {
      RExpr* cond = resolve_expr(g, scope, *condition);
      if (g->hadError)
        return NULL;
      RStmt* b = resolve_branch(g, scope, *body);
      if (g->hadError)
        return NULL;
      RStmtList* pre = New(g->arena, RStmtList, 1);
      return newRStmt(g, RWhile(cond, b, pre));
    }
  }
  return NULL; /* unreachable */
}

static void resolve_list(CGen* g, CgenScope* scope, StmtList* stmts, RStmtList* out) {
  for (isize i = 0; i < stmts->len; i++) {
    RStmt* s = resolve_stmt(g, scope, stmts->data[i]);
    if (g->hadError)
      return;
    *Push(g->arena, out) = s;
  }
}

// ---- Pass 3: lower-logical (L_res -> L_flat) ----

typedef struct {
  RStmtList* pre;
  RExpr* expr;
} LoweredExpr;

static Token tmp_token(int line) {
  Token tok;
  tok.type = TOKEN_IDENTIFIER;
  tok.start = "$t";
  tok.length = 2;
  tok.line = line;
  return tok;
}

static Token bang_token(int line) {
  Token tok;
  tok.type = TOKEN_BANG;
  tok.start = "!";
  tok.length = 1;
  tok.line = line;
  return tok;
}

// Append every statement in src to out.
static void append_list(CGen* g, RStmtList* out, RStmtList* src) {
  for (isize i = 0; i < src->len; i++)
    *Push(g->arena, out) = src->data[i];
}

static void lower_expr(CGen* g, RExpr* e, RStmtList* pre, LoweredExpr* out);

static bool expr_has_effect(CGen* g, RExpr* e) {
  (void)g;
  match(*e) {
    of(RLiteral, value) {
      (void)value;
      return false;
    }
    of(RVar, token, uid) {
      (void)token;
      (void)uid;
      return false;
    }
    of(RAssign, token, uid, value) {
      (void)token;
      (void)uid;
      (void)value;
      return true;
    }
    of(RBinary, left, op, right) {
      (void)op;
      return expr_has_effect(g, *left) || expr_has_effect(g, *right);
    }
    of(RGrouping, inner) {
      return expr_has_effect(g, *inner);
    }
    of(RLogical, left, op, right) {
      (void)left;
      (void)op;
      (void)right;
      return true;
    }
    of(RUnary, op, right) {
      (void)op;
      return expr_has_effect(g, *right);
    }
    of(RCall, callee, paren, args) {
      (void)callee;
      (void)paren;
      (void)args;
      return true;
    }
  }
  return false;
}

// Bind e to a fresh temp appended to pre, returning an RVar over it.
// Fixes C argument evaluation order (otherwise unspecified) to Lox
// left-to-right for effectful operands.
static RExpr* spill_to_tmp(CGen* g, RStmtList* pre, RExpr* e, int line) {
  Token tt = tmp_token(line);
  int tmp = new_uid(g, -1, tt, false);
  *Push(g->arena, pre) = newRStmt(g, RVarDecl(tt, tmp, e));
  return newRExpr(g, RVar(tt, tmp));
}

// Lower e, appending any required pre-statements to the caller-supplied list.
static void lower_expr(CGen* g, RExpr* e, RStmtList* pre, LoweredExpr* out) {
  out->pre = pre;
  out->expr = NULL;
  if (g->hadError)
    return;
  match(*e) {
    of(RLiteral, value) {
      (void)value;
      out->expr = e;
    }
    of(RVar, token, uid) {
      (void)token;
      (void)uid;
      out->expr = e;
    }
    of(RAssign, token, uid, value) {
      LoweredExpr v;
      lower_expr(g, *value, pre, &v);
      if (g->hadError)
        return;
      out->expr = newRExpr(g, RAssign(*token, *uid, v.expr));
    }
    of(RBinary, left, op, right) {
      LoweredExpr l;
      lower_expr(g, *left, pre, &l);
      if (g->hadError)
        return;
      RExpr* le = l.expr;
      if (expr_has_effect(g, le))
        le = spill_to_tmp(g, pre, le, op->line);
      RStmtList* rpre = New(g->arena, RStmtList, 1);
      LoweredExpr r;
      lower_expr(g, *right, rpre, &r);
      if (g->hadError)
        return;
      append_list(g, pre, rpre);
      RExpr* re = r.expr;
      if (expr_has_effect(g, re))
        re = spill_to_tmp(g, pre, re, op->line);
      out->expr = newRExpr(g, RBinary(le, *op, re));
    }
    of(RGrouping, inner) {
      LoweredExpr x;
      lower_expr(g, *inner, pre, &x);
      if (g->hadError)
        return;
      out->expr = newRExpr(g, RGrouping(x.expr));
    }
    of(RLogical, left, op, right) {
      int line = op->line;
      LoweredExpr l;
      lower_expr(g, *left, pre, &l);
      if (g->hadError)
        return;
      Token tt = tmp_token(line);
      int tmp = new_uid(g, -1, tt, false);
      *Push(g->arena, pre) = newRStmt(g, RVarDecl(tt, tmp, l.expr));
      RStmtList* thenBody = New(g->arena, RStmtList, 1);
      LoweredExpr r;
      lower_expr(g, *right, thenBody, &r);
      if (g->hadError)
        return;
      RExpr* tref = newRExpr(g, RVar(tt, tmp));
      // Emitter wraps conditions as `if (!is_falsey(cond))`, so `and`
      // enters on a bare (truthy) temp while `or` enters on `!temp`.
      RExpr* test = (op->type == TOKEN_AND) ? tref : newRExpr(g, RUnary(bang_token(line), tref));
      *Push(g->arena, thenBody) = newRStmt(g, RExpression(newRExpr(g, RAssign(tt, tmp, r.expr))));
      *Push(g->arena, pre) = newRStmt(g, RIf(test, newRStmt(g, RBlock(thenBody)), NULL));
      out->expr = newRExpr(g, RVar(tt, tmp));
    }
    of(RUnary, op, right) {
      LoweredExpr x;
      lower_expr(g, *right, pre, &x);
      if (g->hadError)
        return;
      out->expr = newRExpr(g, RUnary(*op, x.expr));
    }
    of(RCall, callee, paren, args) {
      LoweredExpr c;
      lower_expr(g, *callee, pre, &c);
      if (g->hadError)
        return;
      RExpr* ce = c.expr;
      if (expr_has_effect(g, ce))
        ce = spill_to_tmp(g, pre, ce, paren->line);
      RExprList* callArgs = *args;
      RExprList* rargs = New(g->arena, RExprList, 1);
      for (isize i = 0; i < callArgs->len; i++) {
        RStmtList* apre = New(g->arena, RStmtList, 1);
        LoweredExpr a;
        lower_expr(g, callArgs->data[i], apre, &a);
        if (g->hadError)
          return;
        append_list(g, pre, apre);
        RExpr* ae = a.expr;
        if (expr_has_effect(g, ae))
          ae = spill_to_tmp(g, pre, ae, paren->line);
        *Push(g->arena, rargs) = ae;
      }
      out->expr = newRExpr(g, RCall(ce, *paren, rargs));
    }
  }
}

static void lower_stmt(CGen* g, RStmt* s, RStmtList* out);
static void lower_body(CGen* g, RStmtList* stmts, RStmtList* out);

static void lower_body(CGen* g, RStmtList* stmts, RStmtList* out) {
  for (isize i = 0; i < stmts->len; i++) {
    lower_stmt(g, stmts->data[i], out);
    if (g->hadError)
      return;
  }
}

// Statements lower to a spliced list; branches normalize to Blocks.
static void lower_stmt(CGen* g, RStmt* s, RStmtList* out) {
  if (g->hadError)
    return;
  match(*s) {
    of(RBlock, statements) {
      RStmtList* body = New(g->arena, RStmtList, 1);
      lower_body(g, *statements, body);
      if (g->hadError)
        return;
      *Push(g->arena, out) = newRStmt(g, RBlock(body));
    }
    of(RExpression, expression) {
      RStmtList* pre = New(g->arena, RStmtList, 1);
      LoweredExpr x;
      lower_expr(g, *expression, pre, &x);
      if (g->hadError)
        return;
      append_list(g, out, pre);
      *Push(g->arena, out) = newRStmt(g, RExpression(x.expr));
    }
    of(RFunDecl, name, uid, funcId, params, body) {
      RStmtList* rbody = New(g->arena, RStmtList, 1);
      lower_body(g, *body, rbody);
      if (g->hadError)
        return;
      *Push(g->arena, out) = newRStmt(g, RFunDecl(*name, *uid, *funcId, *params, rbody));
    }
    of(RIf, condition, thenBranch, elseBranch) {
      RStmtList* cpre = New(g->arena, RStmtList, 1);
      LoweredExpr c;
      lower_expr(g, *condition, cpre, &c);
      if (g->hadError)
        return;
      RStmtList* tb = New(g->arena, RStmtList, 1);
      lower_stmt(g, *thenBranch, tb);
      if (g->hadError)
        return;
      RStmt* elseB = NULL;
      if (*elseBranch != NULL) {
        RStmtList* eb = New(g->arena, RStmtList, 1);
        lower_stmt(g, *elseBranch, eb);
        if (g->hadError)
          return;
        elseB = newRStmt(g, RBlock(eb));
      }
      append_list(g, out, cpre);
      *Push(g->arena, out) = newRStmt(g, RIf(c.expr, newRStmt(g, RBlock(tb)), elseB));
    }
    of(RPrint, expression) {
      RStmtList* pre = New(g->arena, RStmtList, 1);
      LoweredExpr x;
      lower_expr(g, *expression, pre, &x);
      if (g->hadError)
        return;
      append_list(g, out, pre);
      *Push(g->arena, out) = newRStmt(g, RPrint(x.expr));
    }
    of(RReturn, keyword, value) {
      if (*value == NULL) {
        *Push(g->arena, out) = s;
      } else {
        RStmtList* pre = New(g->arena, RStmtList, 1);
        LoweredExpr x;
        lower_expr(g, *value, pre, &x);
        if (g->hadError)
          return;
        append_list(g, out, pre);
        *Push(g->arena, out) = newRStmt(g, RReturn(*keyword, x.expr));
      }
    }
    of(RVarDecl, token, uid, initializer) {
      if (*initializer == NULL) {
        *Push(g->arena, out) = s;
      } else {
        RStmtList* pre = New(g->arena, RStmtList, 1);
        LoweredExpr x;
        lower_expr(g, *initializer, pre, &x);
        if (g->hadError)
          return;
        append_list(g, out, pre);
        *Push(g->arena, out) = newRStmt(g, RVarDecl(*token, *uid, x.expr));
      }
    }
    of(RWhile, condition, body, pre) {
      (void)pre;
      RStmtList* cpre = New(g->arena, RStmtList, 1);
      LoweredExpr c;
      lower_expr(g, *condition, cpre, &c);
      if (g->hadError)
        return;
      RStmtList* bb = New(g->arena, RStmtList, 1);
      lower_stmt(g, *body, bb);
      if (g->hadError)
        return;
      // Condition pre-statements stay INSIDE the loop: they re-evaluate
      // every iteration (logical operators lower to temp + branch).
      *Push(g->arena, out) = newRStmt(g, RWhile(c.expr, newRStmt(g, RBlock(bb)), cpre));
    }
  }
}

// ---- Pass 4: emit-c (L_flat -> C text, via arena astr) ----

static bool in_subtree(CGen* g, int f, int root) {
  while (f != root && f > 0)
    f = g->funcParent->data[f - 1];
  return f == root;
}

// Environment for function G: uids used anywhere in G's subtree whose
// declaration lives outside it (globals excluded: accessed directly).
// Ancestor functions between G and the declaration get the same entry,
// forming the upvalue chain.
static void build_envs(CGen* g, CgenEnvList* envs) {
  for (int fid = 1; fid <= g->funcCount; fid++) {
    CgenEnv e = {fid, NULL};
    e.uids = New(g->arena, IntList, 1);
    for (isize i = 0; i < g->useUid->len; i++) {
      int uid = g->useUid->data[i];
      int user = g->useFunc->data[i];
      if (uid <= 0 || !in_subtree(g, user, fid))
        continue;
      if (uid_is_global(g, uid))
        continue;
      if (in_subtree(g, uid_func(g, uid), fid))
        continue;
      bool seen = false;
      for (isize j = 0; j < e.uids->len; j++) {
        if (e.uids->data[j] == uid) {
          seen = true;
          break;
        }
      }
      if (!seen)
        *Push(g->arena, e.uids) = uid;
    }
    *Push(g->arena, envs) = e;
  }
}

static int env_index(CgenEnvList* envs, int funcId, int uid) {
  for (isize i = 0; i < envs->len; i++) {
    if (envs->data[i].funcId != funcId)
      continue;
    for (isize j = 0; j < envs->data[i].uids->len; j++) {
      if (envs->data[i].uids->data[j] == uid)
        return (int)j;
    }
  }
  return -1;
}

static int env_count(CgenEnvList* envs, int funcId) {
  for (isize i = 0; i < envs->len; i++) {
    if (envs->data[i].funcId == funcId)
      return (int)envs->data[i].uids->len;
  }
  return 0;
}

static bool is_tmp(Token tok) {
  return tok.length == 2 && memcmp(tok.start, "$t", 2) == 0;
}

static astr emit_cstring(CGen* g, const char* chars, int length);

static astr emit_name(CGen* g, Token tok, int uid) {
  if (is_tmp(tok))
    return astr_format(g->arena, "_lox_t%d", uid);
  return astr_format(g->arena, "_lox_%.*s_%d", tok.length, tok.start, uid);
}

static astr emit_dname(CGen* g, Token tok, int uid) {
  return astr_format(g->arena, "_lox_def_%.*s_%d", tok.length, tok.start, uid);
}

// Read access for a resolved variable in the function being emitted.
static astr emit_var_read(CGen* g, Token tok, int uid) {
  if (uid < 0) {
    astr out = astr("lox_undefined(");
    out = astr_concat(g->arena, out, emit_cstring(g, tok.start, tok.length));
    return astr_concat(g->arena, out, astr_format(g->arena, ", %d)", tok.line));
  }
  if (uid_is_global(g, uid)) {
    astr out = astr("(");
    out = astr_concat(g->arena, out, emit_dname(g, tok, uid));
    out = astr_concat(g->arena, out, astr(" ? "));
    out = astr_concat(g->arena, out, emit_name(g, tok, uid));
    out = astr_concat(g->arena, out, astr(" : lox_undefined("));
    out = astr_concat(g->arena, out, emit_cstring(g, tok.start, tok.length));
    return astr_concat(g->arena, out, astr_format(g->arena, ", %d))", tok.line));
  }
  if (is_tmp(tok))
    return emit_name(g, tok, uid);
  if (uid_func(g, uid) == g->emitFunc) {
    if (uid_is_boxed(g, uid)) {
      astr out = emit_name(g, tok, uid);
      return astr_concat(g->arena, out, astr("->value"));
    }
    return emit_name(g, tok, uid);
  }
  int k = env_index(g->envs, g->emitFunc, uid);
  if (k < 0) {
    cgen_error(g, tok.line, "internal: missing upvalue");
    return (astr){0};
  }
  return astr_format(g->arena, "(*self->upvalues[%d])", k);
}

// Assignment target for a resolved variable. rhs is already-emitted text.
static astr emit_assign(CGen* g, Token tok, int uid, astr rhs) {
  if (uid < 0) {
    astr out = astr("(");
    out = astr_concat(g->arena, out, rhs);
    out = astr_concat(g->arena, out, astr(", lox_undefined("));
    out = astr_concat(g->arena, out, emit_cstring(g, tok.start, tok.length));
    return astr_concat(g->arena, out, astr_format(g->arena, ", %d))", tok.line));
  }
  if (uid_is_global(g, uid)) {
    astr out = astr("lox_setglobal(&");
    out = astr_concat(g->arena, out, emit_name(g, tok, uid));
    out = astr_concat(g->arena, out, astr(", &"));
    out = astr_concat(g->arena, out, emit_dname(g, tok, uid));
    out = astr_concat(g->arena, out, astr(", "));
    out = astr_concat(g->arena, out, emit_cstring(g, tok.start, tok.length));
    out = astr_concat(g->arena, out, astr_format(g->arena, ", %d, ", tok.line));
    out = astr_concat(g->arena, out, rhs);
    return astr_concat(g->arena, out, astr(")"));
  }
  if (is_tmp(tok) || uid_func(g, uid) == g->emitFunc) {
    astr out = astr("(");
    if (uid_is_boxed(g, uid) && !is_tmp(tok)) {
      out = astr_concat(g->arena, out, emit_name(g, tok, uid));
      out = astr_concat(g->arena, out, astr("->value = "));
    } else {
      out = astr_concat(g->arena, out, emit_name(g, tok, uid));
      out = astr_concat(g->arena, out, astr(" = "));
    }
    out = astr_concat(g->arena, out, rhs);
    return astr_concat(g->arena, out, astr(")"));
  }
  int k = env_index(g->envs, g->emitFunc, uid);
  if (k < 0) {
    cgen_error(g, tok.line, "internal: missing upvalue");
    return (astr){0};
  }
  astr out = astr_format(g->arena, "((*self->upvalues[%d]) = ", k);
  out = astr_concat(g->arena, out, rhs);
  return astr_concat(g->arena, out, astr(")"));
}

// Cell-pointer expression for wiring a captured uid into a new closure,
// from the function being emitted (the declaration site).
static astr emit_cell_ptr(CGen* g, int uid) {
  Token tok = g->uidToken->data[uid - 1];
  if (uid_func(g, uid) == g->emitFunc) {
    astr out = astr("&");
    out = astr_concat(g->arena, out, emit_name(g, tok, uid));
    return astr_concat(g->arena, out, astr("->value"));
  }
  int k = env_index(g->envs, g->emitFunc, uid);
  if (k < 0) {
    cgen_error(g, tok.line, "internal: missing upvalue");
    return (astr){0};
  }
  return astr_format(g->arena, "self->upvalues[%d]", k);
}

static astr emit_indent(CGen* g, int depth) {
  astr out = {0};
  for (int i = 0; i < depth; i++)
    out = astr_concat(g->arena, out, astr("  "));
  return out;
}

typedef Slice(astr) AstrParts;

// Join rendered parts with a single allocation. Statement-list loops must
// use this (not repeated astr_concat, which clones the whole output per
// append and goes quadratic in program size).
static astr join_parts(CGen* g, AstrParts* parts) {
  isize total = 0;
  for (isize i = 0; i < parts->len; i++)
    total += parts->data[i].len;
  char* buf = New(g->arena, char, total, NO_INIT);
  isize off = 0;
  for (isize i = 0; i < parts->len; i++) {
    memcpy(buf + off, parts->data[i].data, (size_t)parts->data[i].len);
    off += parts->data[i].len;
  }
  return (astr){buf, total};
}

static void push_part(CGen* g, AstrParts* parts, astr part) {
  *Push(g->arena, parts) = part;
}

static astr emit_cstring(CGen* g, const char* chars, int length) {
  astr out = astr("\"");
  for (int i = 0; i < length; i++) {
    unsigned char c = (unsigned char)chars[i];
    if (c == '"')
      out = astr_concat(g->arena, out, astr("\\\""));
    else if (c == '\\')
      out = astr_concat(g->arena, out, astr("\\\\"));
    else if (c == '\n')
      out = astr_concat(g->arena, out, astr("\\n"));
    else if (c >= 0x20 && c <= 0x7e)
      out = astr_concat(g->arena, out, astr_format(g->arena, "%c", c));
    else
      out = astr_concat(g->arena, out, astr_format(g->arena, "\\%03o", c));
  }
  return astr_concat(g->arena, out, astr("\""));
}

static astr emit_value(CGen* g, Value v) {
  switch (v.type) {
    case VAL_BOOL:
      return astr_format(g->arena, "BOOL_VAL(%s)", AS_BOOL(v) ? "true" : "false");
    case VAL_NIL:
      return astr("NIL_VAL");
    case VAL_NUMBER:
      // %.17g round-trips every double through strtod; %g does not.
      return astr_format(g->arena, "NUMBER_VAL(%.17g)", AS_NUMBER(v));
    case VAL_OBJ: {
      // Core supports strings only; no other Obj types exist.
      ObjString* s = AS_STRING(v);
      astr out = astr("lox_string(");
      out = astr_concat(g->arena, out, emit_cstring(g, s->chars, s->length));
      out = astr_concat(g->arena, out, astr_format(g->arena, ", %d)", s->length));
      return out;
    }
  }
  return astr("NIL_VAL"); /* unreachable */
}

static astr emit_expr(CGen* g, RExpr* e);

static astr emit_binary(CGen* g, RExpr* l, Token op, RExpr* r) {
  astr la = emit_expr(g, l);
  if (g->hadError)
    return (astr){0};
  astr ra = emit_expr(g, r);
  if (g->hadError)
    return (astr){0};
  switch (op.type) {
    case TOKEN_BANG_EQUAL: {
      astr out = astr("lox_not(lox_equal(");
      out = astr_concat(g->arena, out, la);
      out = astr_concat(g->arena, out, astr(", "));
      out = astr_concat(g->arena, out, ra);
      return astr_concat(g->arena, out, astr("))"));
    }
    case TOKEN_GREATER_EQUAL: {
      astr out = astr_format(g->arena, "lox_not(lox_less(");
      out = astr_concat(g->arena, out, la);
      out = astr_concat(g->arena, out, astr(", "));
      out = astr_concat(g->arena, out, ra);
      out = astr_concat(g->arena, out, astr_format(g->arena, ", %d))", op.line));
      return out;
    }
    case TOKEN_LESS_EQUAL: {
      astr out = astr_format(g->arena, "lox_not(lox_greater(");
      out = astr_concat(g->arena, out, la);
      out = astr_concat(g->arena, out, astr(", "));
      out = astr_concat(g->arena, out, ra);
      out = astr_concat(g->arena, out, astr_format(g->arena, ", %d))", op.line));
      return out;
    }
    default: {
      const char* helper = NULL;
      bool needsLine = true;
      switch (op.type) {
        case TOKEN_PLUS:
          helper = "lox_add";
          break;
        case TOKEN_MINUS:
          helper = "lox_sub";
          break;
        case TOKEN_STAR:
          helper = "lox_mul";
          break;
        case TOKEN_SLASH:
          helper = "lox_div";
          break;
        case TOKEN_EQUAL_EQUAL:
          helper = "lox_equal";
          needsLine = false;
          break;
        case TOKEN_GREATER:
          helper = "lox_greater";
          break;
        case TOKEN_LESS:
          helper = "lox_less";
          break;
        default:
          cgen_error(g, op.line, "internal: bad binary operator");
          return (astr){0};
      }
      astr out = astr_format(g->arena, "%s(", helper);
      out = astr_concat(g->arena, out, la);
      out = astr_concat(g->arena, out, astr(", "));
      out = astr_concat(g->arena, out, ra);
      if (needsLine)
        out = astr_concat(g->arena, out, astr_format(g->arena, ", %d", op.line));
      return astr_concat(g->arena, out, astr(")"));
    }
  }
}

static astr emit_expr(CGen* g, RExpr* e) {
  if (g->hadError)
    return (astr){0};
  match(*e) {
    of(RLiteral, value) {
      return emit_value(g, *value);
    }
    of(RVar, token, uid) {
      return emit_var_read(g, *token, *uid);
    }
    of(RAssign, token, uid, value) {
      astr rhs = emit_expr(g, *value);
      if (g->hadError)
        return (astr){0};
      return emit_assign(g, *token, *uid, rhs);
    }
    of(RBinary, left, op, right) {
      return emit_binary(g, *left, *op, *right);
    }
    of(RGrouping, inner) {
      astr out = astr("(");
      out = astr_concat(g->arena, out, emit_expr(g, *inner));
      return astr_concat(g->arena, out, astr(")"));
    }
    of(RLogical, left, op, right) {
      (void)left;
      (void)right;
      cgen_error(g, op->line, "internal: unlowered logical operator");
      return (astr){0};
    }
    of(RUnary, op, right) {
      if (op->type == TOKEN_BANG) {
        astr out = astr("lox_not(");
        out = astr_concat(g->arena, out, emit_expr(g, *right));
        return astr_concat(g->arena, out, astr(")"));
      }
      if (op->type == TOKEN_MINUS) {
        astr out = astr_format(g->arena, "lox_negate(");
        out = astr_concat(g->arena, out, emit_expr(g, *right));
        out = astr_concat(g->arena, out, astr_format(g->arena, ", %d)", op->line));
        return out;
      }
      cgen_error(g, op->line, "internal: bad unary operator");
      return (astr){0};
    }
    of(RCall, callee, paren, args) {
      RExprList* callArgs = *args;
      astr c = emit_expr(g, *callee);
      if (g->hadError)
        return (astr){0};
      if (callArgs->len == 0) {
        return astr_format(g->arena, "lox_call(%.*s, (Value*)0, 0, %d)", (int)c.len, c.data, paren->line);
      }
      astr out = astr_format(g->arena, "lox_call(%.*s, (Value[]){", (int)c.len, c.data);
      for (isize i = 0; i < callArgs->len; i++) {
        if (i > 0)
          out = astr_concat(g->arena, out, astr(", "));
        out = astr_concat(g->arena, out, emit_expr(g, callArgs->data[i]));
        if (g->hadError)
          return (astr){0};
      }
      return astr_concat(g->arena, out,
                         astr_format(g->arena, "}, %d, %d)", (int)callArgs->len, paren->line));
    }
  }
  return (astr){0}; /* unreachable */
}

static astr emit_stmt(CGen* g, RStmt* s, int depth);

static astr emit_block_body(CGen* g, RStmtList* statements, int depth) {
  astr out = astr("{\n");
  AstrParts* parts = New(g->arena, AstrParts, 1);
  push_part(g, parts, out);
  for (isize i = 0; i < statements->len; i++) {
    astr one = emit_stmt(g, statements->data[i], depth + 1);
    if (g->hadError)
      return (astr){0};
    push_part(g, parts, one);
    push_part(g, parts, astr("\n"));
  }
  push_part(g, parts, emit_indent(g, depth));
  push_part(g, parts, astr("}"));
  return join_parts(g, parts);
}

// Branches are Blocks after lowering; a bare stmt still emits correctly.
static astr emit_branch(CGen* g, RStmt* s, int depth) {
  match(*s) {
    of(RBlock, statements) {
      return emit_block_body(g, *statements, depth);
    }
    of(RExpression, expression) {
      (void)expression;
    }
    of(RFunDecl, name, uid, funcId, params, body) {
      (void)name;
      (void)uid;
      (void)funcId;
      (void)params;
      (void)body;
    }
    of(RIf, condition, thenBranch, elseBranch) {
      (void)condition;
      (void)thenBranch;
      (void)elseBranch;
    }
    of(RPrint, expression) {
      (void)expression;
    }
    of(RReturn, keyword, value) {
      (void)keyword;
      (void)value;
    }
    of(RVarDecl, token, uid, initializer) {
      (void)token;
      (void)uid;
      (void)initializer;
    }
    of(RWhile, condition, body, pre) {
      (void)condition;
      (void)body;
      (void)pre;
    }
  }
  return emit_stmt(g, s, depth);
}

static astr emit_stmt(CGen* g, RStmt* s, int depth) {
  if (g->hadError)
    return (astr){0};
  astr ind = emit_indent(g, depth);
  match(*s) {
    of(RBlock, statements) {
      RStmtList* body = *statements;
      AstrParts* parts = New(g->arena, AstrParts, 1);
      push_part(g, parts, astr_concat(g->arena, ind, astr("{\n")));
      for (isize i = 0; i < body->len; i++) {
        astr one = emit_stmt(g, body->data[i], depth + 1);
        if (g->hadError)
          return (astr){0};
        push_part(g, parts, one);
        push_part(g, parts, astr("\n"));
      }
      push_part(g, parts, emit_indent(g, depth));
      push_part(g, parts, astr("}"));
      return join_parts(g, parts);
    }
    of(RExpression, expression) {
      astr out = astr_concat(g->arena, ind, emit_expr(g, *expression));
      if (g->hadError)
        return (astr){0};
      return astr_concat(g->arena, out, astr(";"));
    }
    of(RFunDecl, name, uid, funcId, params, body) {
      (void)params;
      (void)body;
      // Declare the variable, then build the closure and assign it. Boxed
      // variables (captured, incl. self-recursion) get a heap cell first so
      // the closure can close over it.
      bool isGlobal = uid_is_global(g, *uid);
      bool boxed = uid_is_boxed(g, *uid);
      CgenParamList* fnParams = *params;
      int arity = (int)fnParams->len;
      int nup = env_count(g->envs, *funcId);
      // Compose: [cell decl] ObjCFun* cf; upvalues...; target = OBJ_VAL(cf);
      astr code = astr_concat(g->arena, ind, astr(""));
      if (boxed && !isGlobal) {
        code = astr_concat(g->arena, code, astr("LoxCell* "));
        code = astr_concat(g->arena, code, emit_name(g, *name, *uid));
        code = astr_concat(g->arena, code, astr(" = lox_newcell(NIL_VAL);\n"));
        code = astr_concat(g->arena, code, ind);
      }
      code = astr_concat(
          g->arena, code,
          astr_format(g->arena, "ObjCFun* _lox_cf_%d = newCFun(\"%.*s\", %d, %d, lox_fn_%d, %d);\n", *uid,
                      name->length, name->start, name->length, arity, *uid, nup));
      for (int k = 0; k < nup; k++) {
        int cap = 0;
        for (isize i = 0; i < g->envs->len; i++) {
          if (g->envs->data[i].funcId == *funcId) {
            cap = g->envs->data[i].uids->data[k];
            break;
          }
        }
        code = astr_concat(g->arena, code, ind);
        code = astr_concat(g->arena, code, astr_format(g->arena, "_lox_cf_%d->upvalues[%d] = ", *uid, k));
        code = astr_concat(g->arena, code, emit_cell_ptr(g, cap));
        if (g->hadError)
          return (astr){0};
        code = astr_concat(g->arena, code, astr(";\n"));
      }
      code = astr_concat(g->arena, code, ind);
      if (boxed) {
        code = astr_concat(g->arena, code, emit_name(g, *name, *uid));
        code = astr_concat(g->arena, code, astr_format(g->arena, "->value = OBJ_VAL(_lox_cf_%d);", *uid));
      } else if (isGlobal) {
        code = astr_concat(g->arena, code, emit_name(g, *name, *uid));
        code = astr_concat(g->arena, code, astr_format(g->arena, " = OBJ_VAL(_lox_cf_%d);\n", *uid));
        code = astr_concat(g->arena, code, ind);
        code = astr_concat(g->arena, code, emit_dname(g, *name, *uid));
        code = astr_concat(g->arena, code, astr(" = true;"));
      } else {
        code = astr_concat(g->arena, code, astr("Value "));
        code = astr_concat(g->arena, code, emit_name(g, *name, *uid));
        code = astr_concat(g->arena, code, astr_format(g->arena, " = OBJ_VAL(_lox_cf_%d);", *uid));
      }
      return code;
    }
    of(RIf, condition, thenBranch, elseBranch) {
      astr out = astr_concat(g->arena, ind, astr("if (!lox_is_falsey("));
      out = astr_concat(g->arena, out, emit_expr(g, *condition));
      if (g->hadError)
        return (astr){0};
      out = astr_concat(g->arena, out, astr(")) "));
      out = astr_concat(g->arena, out, emit_branch(g, *thenBranch, depth));
      if (*elseBranch != NULL) {
        out = astr_concat(g->arena, out, astr(" else "));
        out = astr_concat(g->arena, out, emit_branch(g, *elseBranch, depth));
      }
      return out;
    }
    of(RPrint, expression) {
      astr out = astr_concat(g->arena, ind, astr("lox_print("));
      out = astr_concat(g->arena, out, emit_expr(g, *expression));
      if (g->hadError)
        return (astr){0};
      return astr_concat(g->arena, out, astr(");"));
    }
    of(RReturn, keyword, value) {
      (void)keyword;
      astr out = astr_concat(g->arena, ind, astr("return "));
      if (*value != NULL) {
        out = astr_concat(g->arena, out, emit_expr(g, *value));
        if (g->hadError)
          return (astr){0};
      } else {
        out = astr_concat(g->arena, out, astr("NIL_VAL"));
      }
      return astr_concat(g->arena, out, astr(";"));
    }
    of(RVarDecl, token, uid, initializer) {
      bool isGlobal = uid_is_global(g, *uid);
      bool boxed = uid_is_boxed(g, *uid);
      astr init = astr("NIL_VAL");
      if (*initializer != NULL) {
        init = emit_expr(g, *initializer);
        if (g->hadError)
          return (astr){0};
      }
      if (isGlobal) {
        astr out = astr_concat(g->arena, ind, emit_name(g, *token, *uid));
        out = astr_concat(g->arena, out, astr(" = "));
        out = astr_concat(g->arena, out, init);
        out = astr_concat(g->arena, out, astr(";\n"));
        out = astr_concat(g->arena, out, ind);
        out = astr_concat(g->arena, out, emit_dname(g, *token, *uid));
        return astr_concat(g->arena, out, astr(" = true;"));
      }
      astr out = astr_concat(g->arena, ind, astr(""));
      if (boxed) {
        out = astr_concat(g->arena, out, astr("LoxCell* "));
        out = astr_concat(g->arena, out, emit_name(g, *token, *uid));
        out = astr_concat(g->arena, out, astr(" = lox_newcell("));
        out = astr_concat(g->arena, out, init);
        return astr_concat(g->arena, out, astr(");"));
      }
      out = astr_concat(g->arena, out, astr("Value "));
      out = astr_concat(g->arena, out, emit_name(g, *token, *uid));
      out = astr_concat(g->arena, out, astr(" = "));
      out = astr_concat(g->arena, out, init);
      return astr_concat(g->arena, out, astr(";"));
    }
    of(RWhile, condition, body, pre) {
      RStmtList* preStmts = *pre;
      AstrParts* parts = New(g->arena, AstrParts, 1);
      push_part(g, parts, astr_concat(g->arena, ind, astr("for (;;) {\n")));
      for (isize i = 0; i < preStmts->len; i++) {
        push_part(g, parts, emit_stmt(g, preStmts->data[i], depth + 1));
        if (g->hadError)
          return (astr){0};
        push_part(g, parts, astr("\n"));
      }
      push_part(g, parts, emit_indent(g, depth + 1));
      push_part(g, parts, astr("if (lox_is_falsey("));
      push_part(g, parts, emit_expr(g, *condition));
      if (g->hadError)
        return (astr){0};
      push_part(g, parts, astr(")) break;\n"));
      push_part(g, parts, emit_indent(g, depth + 1));
      push_part(g, parts, emit_branch(g, *body, depth + 1));
      push_part(g, parts, astr("\n"));
      push_part(g, parts, ind);
      push_part(g, parts, astr("}"));
      return join_parts(g, parts);
    }
  }
  return (astr){0}; /* unreachable */
}

// ---- Driver ----

typedef struct {
  Token name;
  int uid;
  int funcId;
  CgenParamList* params;
  RStmtList* body;
} CgenFun;

typedef Slice(CgenFun) CgenFunList;

static void collect_funs_stmt(CGen* g, RStmt* s, CgenFunList* out);
static void collect_funs_list(CGen* g, RStmtList* stmts, CgenFunList* out) {
  for (isize i = 0; i < stmts->len; i++)
    collect_funs_stmt(g, stmts->data[i], out);
}

static void collect_funs_stmt(CGen* g, RStmt* s, CgenFunList* out) {
  match(*s) {
    of(RBlock, statements) {
      collect_funs_list(g, *statements, out);
    }
    of(RExpression, expression) {
      (void)expression;
    }
    of(RFunDecl, name, uid, funcId, params, body) {
      CgenFun f = {*name, *uid, *funcId, *params, *body};
      *Push(g->arena, out) = f;
      collect_funs_list(g, *body, out);
    }
    of(RIf, condition, thenBranch, elseBranch) {
      (void)condition;
      collect_funs_stmt(g, *thenBranch, out);
      if (*elseBranch != NULL)
        collect_funs_stmt(g, *elseBranch, out);
    }
    of(RPrint, expression) {
      (void)expression;
    }
    of(RReturn, keyword, value) {
      (void)keyword;
      (void)value;
    }
    of(RVarDecl, token, uid, initializer) {
      (void)token;
      (void)uid;
      (void)initializer;
    }
    of(RWhile, condition, body, pre) {
      (void)condition;
      RStmtList* preStmts = *pre;
      collect_funs_stmt(g, *body, out);
      for (isize i = 0; i < preStmts->len; i++)
        collect_funs_stmt(g, preStmts->data[i], out);
    }
  }
}

static astr emit_function(CGen* g, CgenFun* f) {
  astr code = astr_format(
      g->arena, "static Value lox_fn_%d(ObjCFun* self, Value* args, int argCount, int callLine) {\n", f->uid);
  code = astr_concat(g->arena, code,
                     astr_format(g->arena, "  if (argCount != %d) lox_arity(callLine, %d, argCount);\n",
                                 (int)f->params->len, (int)f->params->len));
  for (isize i = 0; i < f->params->len; i++) {
    CgenParam* p = &f->params->data[i];
    Token tok = {p->name.type, p->name.start, p->name.length, p->name.line};
    if (uid_is_boxed(g, p->uid)) {
      code = astr_concat(g->arena, code, astr("  LoxCell* "));
      code = astr_concat(g->arena, code, emit_name(g, tok, p->uid));
      code = astr_concat(g->arena, code, astr_format(g->arena, " = lox_newcell(args[%d]);\n", (int)i));
    } else {
      code = astr_concat(g->arena, code, astr("  Value "));
      code = astr_concat(g->arena, code, emit_name(g, tok, p->uid));
      code = astr_concat(g->arena, code, astr_format(g->arena, " = args[%d];\n", (int)i));
    }
  }
  g->emitFunc = f->funcId;
  AstrParts* parts = New(g->arena, AstrParts, 1);
  push_part(g, parts, code);
  for (isize i = 0; i < f->body->len; i++) {
    astr one = emit_stmt(g, f->body->data[i], 1);
    if (g->hadError)
      return (astr){0};
    push_part(g, parts, one);
    push_part(g, parts, astr("\n"));
  }
  g->emitFunc = 0;
  code = join_parts(g, parts);
  return astr_concat(g->arena, code, astr("  return NIL_VAL;\n}\n"));
}

CgenResult cgen_compile(Arena* arena, StmtList* prog) {
  CgenResult fail = {{0}, false};
  CGen g = {0};
  g.arena = arena;
  g.uidFunc = New(arena, IntList, 1);
  g.uidGlobal = New(arena, BoolList, 1);
  g.uidCaptured = New(arena, BoolList, 1);
  g.uidToken = New(arena, TokenList, 1);
  g.useUid = New(arena, IntList, 1);
  g.useFunc = New(arena, IntList, 1);
  g.funcParent = New(arena, IntList, 1);

  for (isize i = 0; i < prog->len; i++)
    check_stmt(&g, prog->data[i], false);
  if (g.hadError)
    return fail;

  CgenScope* top = new_scope(&g, NULL, 0);
  top->isScriptDirect = true;

  // Predefine clock as a script global so `clock` / `clock()` resolve.
  Token clockTok;
  clockTok.type = TOKEN_IDENTIFIER;
  clockTok.start = "clock";
  clockTok.length = 5;
  clockTok.line = 1;
  g.clockUid = new_uid(&g, 0, clockTok, true);
  CgenBinding clockBind = {clockTok.start, clockTok.length, g.clockUid};
  *Push(arena, top->bindings) = clockBind;

  // Hoist top-level function names so siblings (incl. forward references
  // like mutual recursion) resolve. Reuses the same AST node on resolve.
  for (isize i = 0; i < prog->len; i++) {
    match(*prog->data[i]) {
      of(Function, name, params, body) {
        (void)params;
        (void)body;
        CgenBinding b = {name->start, name->length, 0};
        bool already = false;
        for (isize j = 0; j < top->bindings->len; j++) {
          if (top->bindings->data[j].start == name->start && top->bindings->data[j].length == name->length) {
            already = true;
            break;
          }
        }
        if (!already) {
          int uid = new_uid(&g, 0, *name, true);
          b.uid = uid;
          *Push(arena, top->bindings) = b;
        }
      }
      otherwise {
      }
    }
  }

  RStmtList* resolved = New(arena, RStmtList, 1);
  resolve_list(&g, top, prog, resolved);
  if (g.hadError)
    return fail;

  RStmtList* flat = New(arena, RStmtList, 1);
  lower_body(&g, resolved, flat);
  if (g.hadError)
    return fail;

  g.envs = New(arena, CgenEnvList, 1);
  build_envs(&g, g.envs);

  // File-scope C functions (collected pre-order, nested included).
  CgenFunList* funs = New(arena, CgenFunList, 1);
  collect_funs_list(&g, flat, funs);

  astr code = astr("#include \"loxrt.h\"\n\n");
  // Script globals (script-direct uids with declarations).
  for (int uid = 1; uid <= g.uid; uid++) {
    if (!uid_is_global(&g, uid))
      continue;
    if (uid == g.clockUid) {
      bool used = false;
      for (isize i = 0; i < g.useUid->len; i++) {
        if (g.useUid->data[i] == uid) {
          used = true;
          break;
        }
      }
      if (!used)
        continue;
    }
    Token tok = g.uidToken->data[uid - 1];
    code = astr_concat(arena, code, astr("static Value "));
    code = astr_concat(arena, code, emit_name(&g, tok, uid));
    code = astr_concat(arena, code, astr(";\nstatic bool "));
    code = astr_concat(arena, code, emit_dname(&g, tok, uid));
    code = astr_concat(arena, code, astr(";\n"));
  }
  code = astr_concat(arena, code, astr("\n"));
  for (isize i = 0; i < funs->len; i++) {
    code = astr_concat(
        arena, code,
        astr_format(arena,
                    "static Value lox_fn_%d(ObjCFun* self, Value* args, int argCount, int callLine);\n",
                    funs->data[i].uid));
  }
  code = astr_concat(arena, code, astr("\n"));
  for (isize i = 0; i < funs->len; i++) {
    code = astr_concat(arena, code, emit_function(&g, &funs->data[i]));
    if (g.hadError)
      return fail;
    code = astr_concat(arena, code, astr("\n"));
  }

  g.emitFunc = 0;
  code = astr_concat(arena, code, astr("int main(void) {\n  lox_init();\n"));
  // Clock singleton into its global before user code runs.
  for (isize i = 0; i < g.useUid->len; i++) {
    if (g.useUid->data[i] == g.clockUid) {
      Token tok = g.uidToken->data[g.clockUid - 1];
      code = astr_concat(arena, code, astr("  "));
      code = astr_concat(arena, code, emit_name(&g, tok, g.clockUid));
      code = astr_concat(arena, code, astr(" = lox_clock();\n  "));
      code = astr_concat(arena, code, emit_dname(&g, tok, g.clockUid));
      code = astr_concat(arena, code, astr(" = true;\n"));
      break;
    }
  }
  AstrParts* parts = New(arena, AstrParts, 1);
  push_part(&g, parts, code);
  for (isize i = 0; i < flat->len; i++) {
    astr one = emit_stmt(&g, flat->data[i], 1);
    if (g.hadError)
      return fail;
    push_part(&g, parts, one);
    push_part(&g, parts, astr("\n"));
  }
  push_part(&g, parts, astr("  lox_free();\n  return 0;\n}\n"));
  code = join_parts(&g, parts);

  CgenResult ok = {code, true};
  return ok;
}

#endif
