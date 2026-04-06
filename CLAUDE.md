# Dala (달아) - Project Context

## Project Overview
Lua 5.5.0 fork with modern language features. Repository: corbie79/lua5g
Branch: claude/lua-types-and-classes-RvWYf (merged to main via PRs #1-#6)

## What's Been Implemented (58 commits)

### Language Features
- Type annotations (: number, : string, etc.) with compile-time + runtime checking (OP_TYPECHECK)
- any/unknown dynamic types, generics Array<T>, nullable T?, union T|U, type aliases
- class/extends with override/super keywords
- interface/implements (compile-time method enforcement)
- enum (auto-increment + explicit values)
- Access control: public/private/protected/readonly
- Getter/setter via property keyword
- abstract/static/operator overloading
- declare class for C bindings, .d.lua declaration files
- try/except/finally (compiles to pcall)
- match/case pattern matching (compiles to if/elseif)
- Lambda: |x| x * 2
- String interpolation: f"hello {name}"
- Nullable chaining: a?.b?.c
- Table destructuring: local {x, y} = t
- import "module" / import name from "module"
- async/await (coroutine wrapper)
- RTTI: instanceof(), classname(), classof(), parentof()
- --strict (all locals need types) / --legacy (ignore types) modes

### All keywords are CONTEXTUAL
- Work as keywords at statement start: class Foo, try ... end
- Work as names after . and : : t.class = 1, s:match("pattern")
- Implemented via lexer check: ls->t.token != '.' && ls->t.token != ':'
- 'match' is fully contextual (not reserved at all, detected in TK_NAME)
- Others (class/extends/interface/enum/try) removed from reserved words, detected in TK_NAME

### JIT Compiler
- x86-64: int+float, SSE2, register promotion (≈LuaJIT perf)
- x86-32: int+float, SSE2
- ARMv7: int+float, VFP (QEMU verified)
- ARM64: emitter complete, runtime needs real hardware
- Auto hot loop detection (threshold=100)
- Compile loop fully architecture-abstracted (jit_emit_* functions)
- Only compiles pure arithmetic loops (safe fallback for everything else)

### Tools & Libraries
- C binding API: luaL_newclass, luaL_newsubclass, luaL_pushinstance, luaL_isinstance
- mathx.farray: native C float arrays (10-20x faster than Lua tables)
- trace: error tracing with 3 modes (full/safe/encoded for live services)
- .ldb debug symbol files
- luac -t (strip types) / luac -s (full strip)
- Built-in profiler (require("profile"))
- Built-in test framework (require("test") with describe/it/expect)
- Document generator (tools/docgen.lua)

### VSCode Extension (vscode-dala/)
- TextMate grammar for syntax highlighting
- 12 code snippets
- LSP server (tools/lsp/server.lua) - autocomplete, hover
- DAP debugger (tools/lsp/debugger.lua) - breakpoints, step, variables
- REPL Tab completion (keywords + globals + table.field)

### Icon
- assets/dala-icon.svg: Korean moon rabbit style
- Black silhouettes on golden moon (#F5C131)
- Two rabbits pounding rice cake with mortar

## Key Files Modified from Lua 5.5
- src/llex.h/c - contextual keywords, TString pointers for each
- src/lparser.h/c - type annotations, class/interface/enum/try/match parsing, access control
- src/lparser.h - FuncState.classctx, Vardesc.type_annotation
- src/lobject.h - LocVar.typename_, Proto.jit, Proto.hotcount
- src/lopcodes.h/c - OP_TYPECHECK with TYPEID_* constants
- src/lvm.c - OP_TYPECHECK handler, JIT dispatch in OP_FORPREP
- src/ljit.h/c - JIT compiler (4 architecture backends)
- src/ldebug.h/c - luaG_typecheckerror
- src/lcode.h/c - luaK_stringK exported
- src/ldump.c/lundump.c - typename_ serialization, type stripping
- src/ldo.c - precallC optimization
- src/lua.c - REPL completion, -d flag, --strict/--legacy
- src/lstate.h/c - global_State.typemode
- src/luaconf.h - DALA_MODE_* constants, LUA_CLASS_RELEASE

## New Files
- src/ljit.h/c - JIT compiler
- src/lmathx.c - mathx library + farray
- src/ltrace.c - trace library
- src/lprofile.c - profiler
- src/ltest.c - test framework
- src/lldb.c - debug symbols
- src/lauxlib.h/c - class binding API (luaL_newclass etc.)
- tools/lsp/server.lua - LSP server
- tools/lsp/debugger.lua - DAP debugger
- tools/docgen.lua - doc generator
- vscode-dala/* - VSCode extension
- assets/dala-icon.svg - icon
- docs/*.md - documentation
- tests/*.lua - test suites
- bench/*.lua, bench/cbench.c - benchmarks
- examples/class_c_binding.c - C binding example

## Build
```bash
make linux                    # standard build
make linux MYCFLAGS="-DLUA_USE_READLINE" MYLIBS="-lreadline"  # with REPL completion
gcc -m32 ... -o dala_x86      # x86-32
arm-linux-gnueabihf-gcc -static ... -o dala_arm  # ARMv7
```

## Known Issues
- class_test.lua and regression_test.lua have some failures after contextual keyword changes
- ARM64 JIT: emitter works but runtime crashes in QEMU (likely icache issue)
- match keyword: removed from reserved words entirely to fix string:match() conflict
