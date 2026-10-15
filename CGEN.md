# cgen: AST→C backend (full Lox except OOP)

Decision: compile everything except classes (`Class`/`Get`/`Set`/`This`/
`Super` and methods) to standalone C via four single-task passes over
arena-allocated IR, reusing the `Value`/`Obj` runtime. OOP constructs fail
fast with `unsupported '…'` plus line.

## Pass contracts (validated by prototype, then implemented)

- `check-core`: `L_src` → `L_core` or error. Rejects `Class`/`Get`/`Set`/
  `Super`/`This`. (`for` never arrives: the parser already desugars it to
  `Block`/`While`.) `Return` is allowed only inside `fun` ("Can't return
  from top-level code.").
- `resolve-scopes`: `L_core` → `L_res`. Every binding becomes
  `_lox_<name>_<uid>`; initializers resolve in the enclosing scope; a later
  declaration in the same SCRIPT scope shadows earlier ones (global
  semantics). Same-scope duplicates in local scopes and self-initializers
  are compile errors, mirroring the VM. Top-level `fun` names are hoisted
  so siblings resolve forward references (mutual recursion); capture
  analysis marks every non-global uid used outside its declaring function.
- `lower-logical`: `L_res` → `L_flat`. Expression lowering returns
  `[preStmts, expr]`; RHS pre-statements land *inside* the branch; `and` enters
  on a bare temp, `or` on `!temp`; branches normalize to `Block`s. Call
  args lower left-to-right (callee first); function bodies and `return`
  values recurse.
- `emit-c`: `L_flat` → C text (built with arena `astr` inside the compiler; the
  generated C is plain code). Every Lox function becomes a file-scope C
  function plus an `ObjCFun` value (new `OBJ_CFUN` runtime type for identity,
  `<fn name>` printing and pointer equality). Captured variables live in
  `LoxCell` heap cells shared by pointer (one cell per declaration
  execution, so loop-body closures stay per-iteration); upvalue chains form
  through ancestor envs. Script globals carry `defined` flags checked on
  read/assign, matching the VM's `OP_GET`/`OP_SET_GLOBAL` failures even for
  forward references. Calls go through `lox_call` (arity errors and
  can-only-call with VM-identical messages); `clock` is a predefined global
  holding the shared native object. Temp-numbering pre-pass; truthiness
  wrapping only in `If`/`While` (`if (!lox_is_falsey(…))`); runtime failures
  carry token lines and exit 70 with VM-identical messages (single-frame
  traceback: `[line N] in script`).

## Settled questions

- C argument evaluation order is unspecified, so effectful operands are
  spilled to temps left-to-right in `lower-logical` (`RBinary`/`RCall`);
  pure operands stay inline. Proven by differential runs against `./main`.
- String literals live on the VM heap via `copyString`, freed by `lox_free`.
- Short-circuit laziness survives lowering both polarities (`[false,0]`,
  `[true,0]` with the RHS never running).
- Generated C never triggers the mark-sweep collector (`vm.nextGC = SIZE_MAX`
  in `lox_init`): every live `Value` sits in C locals, C globals or malloc'd
  `LoxCell`s, none of which the collector traces. Allocations are still
  freed by `lox_free` at exit; collection is unobservable except timing.
- Transitive captures need no runtime chain walk: each function's env is
  computed from whole-subtree uses, so ancestor frames re-export cells their
  nested closures close over.

## Verification bar

`python3 /tmp/opencode/cgen_runtests.py`: 240 files → 166 PASS, 74
out-of-scope (OOP only: classes/properties/setters, all rejected as
designed), zero crashes/miscompiles/`cc` failures, zero known divergences
(the old resolver gaps — same-scope redeclare and self-initializer — now
mirror the VM as compile errors). Undefined names lower to a `lox_undefined`
runtime abort (message + exit 70), matching the VM's `OP_GET`/`OP_SET_GLOBAL`
failure even when unreachable.

Standing VM-parity gate `python3 /tmp/opencode/parity_runtests.py`
(VM binary vs cgen binary, same programs): 113 PARITY, 126 both-reject
(exit 65 on both sides, OOP and syntax cases), 1 DIAG-DIFF
(`function/local_mutual_recursion`: identical message and exit 70, VM prints
the deeper call-stack traceback), zero behavior diffs either direction.

## Pointers

- Implementation: `cgen.h`, `cgen.c`, `loxrt.h`, `loxrt.c`, `main.c --emit-c`.
- Throwaway prototypes (primary sources, capture to a branch per skill rule):
  C spike in `/tmp/opencode/cspike/` (`run.sh` reproduces), pass-pipeline demo
  `cgen-pipeline.proto.html` (double-click; module checks via
  `/tmp/opencode/proto_check.js`, shell via `/tmp/opencode/proto_shell_test.js`).
- Grammar: `book/appendix-i.md` in `craftinginterpreters`; suite runner this
  record's bar mirrors: `tool/bin/test.dart` (language=c).
