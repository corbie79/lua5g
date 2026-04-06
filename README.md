# Lua5g

**Lua 5.5 fork with type system, classes, JIT compiler, and modern language features.**

Lua5g extends Lua 5.5.0 with TypeScript/Luau-inspired type annotations, a full class system with inheritance and access control, a lightweight JIT compiler for x86-64/x86/ARMv7, and modern syntax features — all while maintaining full backward compatibility with standard Lua.

## Quick Start

```bash
# Build
make linux

# Run
./src/lua script.lua

# REPL (with readline tab-completion)
make linux MYCFLAGS="-DLUA_USE_READLINE" MYLIBS="-lreadline"
./src/lua
```

## Features at a Glance

```lua
-- Type annotations (compile-time + runtime checked)
local name: string = "Lua5g"
local count: number = 42

-- Classes with inheritance
class Animal
  private _health: number
  readonly species: string

  function new(self, name: string, species: string)
    local inst = setmetatable({}, self)
    inst.name = name
    inst.species = species
    inst._health = 100
    return inst
  end

  function speak(self): string
    return self.name .. " speaks"
  end
end

class Dog extends Animal
  override function speak(self): string
    return super.speak(self) .. " and barks"
  end
end

-- Pattern matching
match status
  case 200 then print("OK")
  case 404 then print("Not Found")
  case _ then print("Unknown")
end

-- Try/except/finally
try
  dangerous_operation()
except err then
  print("Error:", err)
finally
  cleanup()
end

-- Lambda, f-string, nullable chaining
local double = |x| x * 2
print(f"Result: {double(21)}")
local name = user?.profile?.name
```

## Table of Contents

- [Building](#building)
- [Type System](#type-system)
- [Class System](#class-system)
- [Pattern Matching](#pattern-matching)
- [Exception Handling](#exception-handling)
- [Modern Syntax](#modern-syntax)
- [JIT Compiler](#jit-compiler)
- [C Binding API](#c-binding-api)
- [Built-in Libraries](#built-in-libraries)
- [Tooling](#tooling)
- [VSCode Extension](#vscode-extension)
- [Architecture](#architecture)

## Building

### Linux (x86-64)
```bash
make linux
```

### With readline (REPL tab-completion)
```bash
make linux MYCFLAGS="-DLUA_USE_READLINE" MYLIBS="-lreadline"
```

### Release build (strip type checks)
```bash
make linux MYCFLAGS="-DLUA_CLASS_RELEASE"
```

### Cross-compile for ARM
```bash
# ARMv7
arm-linux-gnueabihf-gcc -std=gnu99 -O2 -DLUA_USE_LINUX -static \
  $(ls src/*.c | grep -v luac.c) -o lua5g_arm -lm -ldl

# x86 32-bit
gcc -m32 -std=gnu99 -O2 -DLUA_USE_LINUX \
  $(ls src/*.c | grep -v luac.c) -o lua5g_x86 -lm -ldl
```

## Type System

### Basic Annotations
```lua
local x: number = 42
local y: string = "hello"
local z: boolean = true
local t: table = {}
local f: function = function() end
local a: any = "anything"       -- no type checking
local u: unknown = some_value   -- tracked in debug info
local n: number? = nil          -- nullable
```

### Compile-time Checking
```lua
local x: number = "wrong"  -- ERROR at load time:
-- type error: 'number' expected for variable 'x', got 'string'
```

### Runtime Checking (OP_TYPECHECK)
```lua
local function get_value() return "not a number" end
local x: number = get_value()  -- ERROR at runtime
```

### Generic Type Syntax
```lua
local list: Array<number> = {1, 2, 3}
local map: Map<string, number> = {x = 1}
```

### Type Aliases
```lua
type Point = {x: number, y: number}
type Callback = (number, string)
```

### Stripping Types for Release
```bash
luac -t -o release.luac script.lua   # strip types, keep debug names
luac -s -o minimal.luac script.lua   # strip everything
```

## Class System

### Basic Class
```lua
class Vec2
  function new(self, x: number, y: number)
    local inst = setmetatable({}, self)
    inst.x = x
    inst.y = y
    return inst
  end

  function length(self): number
    return math.sqrt(self.x^2 + self.y^2)
  end
end

local v = Vec2:new(3, 4)
print(v:length())  -- 5.0
```

### Inheritance
```lua
class Shape
  function area(self): number return 0 end
end

class Circle extends Shape
  override function area(self): number
    return math.pi * self.radius^2
  end
end
```

### Override & Super
```lua
class Dog extends Animal
  override function speak(self): string
    return super.speak(self) .. " and barks"
  end
end
-- Without 'override': compile error if parent has same method
-- With 'override' on non-existent: compile error
```

### Access Control
```lua
class Account
  public name: string
  private _balance: number
  protected _id: number
  readonly currency: string

  property balance: number
    get(self) return self._balance end
    set(self, val)
      if val < 0 then error("negative balance") end
      self._balance = val
    end
  end

  function new(self, name, bal)
    local inst = setmetatable({}, self)
    inst.name = name
    inst._balance = bal
    inst.currency = "USD"
    return inst
  end
end

local acc = Account:new("Alice", 100)
print(acc.balance)    -- getter → 100
acc.balance = 50      -- setter
acc.currency = "EUR"  -- ERROR: readonly
print(acc._balance)   -- ERROR: private (outside class methods)
```

### Static & Abstract
```lua
class MathHelper
  static function add(a, b) return a + b end
end
MathHelper.add(1, 2)  -- no self needed

class Shape
  abstract function area(self): number
  abstract function perimeter(self): number
end
```

### Operator Overloading
```lua
class Vec2
  operator + (a, b)
    return Vec2:new(a.x + b.x, a.y + b.y)
  end
  operator tostring (self)
    return f"({self.x}, {self.y})"
  end
end
print(Vec2:new(1,2) + Vec2:new(3,4))  -- (4, 6)
```

### Interface & Implements
```lua
interface Drawable
  function draw(self): nil
  function getColor(self): string
end

class Circle implements Drawable
  function draw(self) ... end
  function getColor(self) return "red" end
  -- Missing method → compile error:
  -- "class 'Circle' missing method 'X' required by interface 'Drawable'"
end
```

### Enum
```lua
enum Direction
  UP
  DOWN
  LEFT
  RIGHT
end
print(Direction.UP)    -- 1
print(Direction.RIGHT) -- 4

enum HttpStatus
  OK = 200
  NOT_FOUND = 404
  ERROR = 500
end
```

### Declare Class (for C bindings)
```lua
-- vec.d.lua
declare class Vec2
  function new(self, x: number, y: number): Vec2
  function length(self): number
end
```
```bash
lua5g -d vec.d.lua script.lua
```

## Pattern Matching

```lua
match value
  case 1 then print("one")
  case "hello" then print("greeting")
  case _ then print("default")
end

-- With enum
enum Color RED GREEN BLUE end
match color
  case Color.RED then set_color(255, 0, 0)
  case Color.GREEN then set_color(0, 255, 0)
  case Color.BLUE then set_color(0, 0, 255)
end
```

## Exception Handling

```lua
try
  local data = load_file("config.json")
  process(data)
except err then
  print("Failed:", err)
finally
  cleanup()  -- always runs
end
```

## Modern Syntax

### Lambda
```lua
local double = |x| x * 2
local add = |x, y| x + y
local transform = |list, fn| ...

-- In higher-order functions
table.sort(items, |a, b| a.score > b.score)
```

### String Interpolation
```lua
local name = "World"
print(f"Hello {name}!")
print(f"1 + 2 = {result}")
```

### Nullable Chaining
```lua
local name = user?.profile?.name  -- nil if any part is nil
local len = str?.len              -- nil if str is nil
```

### Table Destructuring
```lua
local {x, y, z} = get_position()
local {name, age} = user
```

### Import
```lua
import "math"                    -- local math = require("math")
import json from "dkjson"        -- local json = require("dkjson")
```

### Async/Await
```lua
local result = async_run(function()
  local data = fetch("http://api.example.com")
  await()
  return process(data)
end)
```

## JIT Compiler

Lua5g includes a lightweight method JIT that compiles hot numeric for-loops to native machine code.

### Supported Architectures

| Architecture | Integer | Float | Status |
|-------------|---------|-------|--------|
| x86-64 | ✅ 6-20x | ✅ SSE2 | Production |
| x86-32 | ✅ | ✅ SSE2 | Tested |
| ARMv7 | ✅ | ✅ VFP | QEMU verified |
| ARM64 | Emitter ready | Emitter ready | Needs HW test |

### Auto JIT
```lua
-- Loops automatically JIT-compiled after 100 iterations
local function sum(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
-- First 100 calls: interpreter
-- After: native machine code (6-20x faster)
```

### Manual JIT
```lua
local ok = __jit_compile(sum)   -- force compile
print(__jit_status())            -- "x86-64" / "arm" / "none"
```

### Benchmark
```
50M integer sum:
  LuaJIT (ON):   0.035 sec
  Lua5g JIT:     0.037 sec  ← matches LuaJIT
  LuaJIT (OFF):  0.203 sec
  Lua5g interp:  0.229 sec
```

## C Binding API

### Creating Classes from C
```c
static int vec2_new(lua_State *L) {
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  luaL_pushinstance(L, "Vec2");
  lua_pushnumber(L, x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, y); lua_setfield(L, -2, "y");
  return 1;
}

static const luaL_Reg vec2_methods[] = {
  {"new", vec2_new},
  {"length", vec2_length},
  {NULL, NULL}
};

// Register
luaL_newclass(L, "Vec2", vec2_methods);

// With inheritance
luaL_newsubclass(L, "Vec3", "Vec2", vec3_methods);
```

### API Functions
```c
luaL_newclass(L, name, methods)       // create class
luaL_newsubclass(L, name, parent, m)  // class with extends
luaL_pushinstance(L, classname)       // create instance
luaL_isinstance(L, idx, classname)    // instanceof check
luaL_checkinstance(L, arg, classname) // arg type check
luaL_setupclass(L)                    // set up access control
```

## Built-in Libraries

### mathx - High Performance Math
```lua
local mathx = require("mathx")

-- Native float arrays (C memory, zero Lua overhead)
local a = mathx.farray(1000000)
a:fill_range(1, 1)
a:map_sqrt()              -- 10x faster than Lua loop
local s = a:sum()         -- 10x faster
local d = a:dot(b)        -- 15x faster
b:saxpy(2.5, a)           -- 20x faster
```

### trace - Error Tracing
```lua
local trace = require("trace")

-- Development: show all variable values
trace.enable("full")

-- Production: obfuscated (needs .ldb to decode)
trace.enable("encoded")

-- Client-safe: only trace ID
trace.enable("safe")

local ok, err = trace.pcall(function()
  dangerous_code()
end)
```

### profile - Profiler
```lua
local profile = require("profile")
profile.start()
run_code()
profile.stop()
profile.report(10)  -- top 10 hottest functions
```

### test - Test Framework
```lua
local test = require("test")

test.describe("Calculator", function()
  test.it("should add", function()
    test.expect(1 + 1).toBe(2)
  end)
  test.it("should handle types", function()
    test.expect("hello").toBeType("string")
    test.expect(nil).toBeNil()
    test.expect("hello world").toContain("world")
  end)
end)

test.summary()
```

### ldb - Debug Symbols
```lua
local ldb = require("ldb")
ldb.generate("app.ldb", loadfile("app.lua"))  -- save symbols
-- Ship: app.luac (stripped) + app.ldb (symbols for crash analysis)
```

## Tooling

### luac Options
```bash
luac -l script.lua           # disassemble (shows TYPECHECK etc.)
luac -t -o out.luac in.lua   # strip types (TYPECHECK → NOP)
luac -s -o out.luac in.lua   # full strip (types + debug info)
```

### Declaration Files
```bash
lua5g -d types.d.lua script.lua   # load type declarations before script
```

### Document Generator
```bash
lua5g tools/docgen.lua api.d.lua > api.md
```

## VSCode Extension

### Installation
```bash
cp -r vscode-lua5g ~/.vscode/extensions/
cd ~/.vscode/extensions/vscode-lua5g
npm install vscode-languageclient
```

### Features
- **Syntax highlighting**: all Lua5g keywords, types, modifiers
- **12 code snippets**: class, try, match, lambda, enum, etc.
- **LSP server**: autocomplete, hover info, diagnostics
- **DAP debugger**: breakpoints, step, variable inspection
- **Code folding**: class/function/if/try blocks
- **Smart indentation**: auto-indent for all blocks

### Settings
```json
{
  "lua5g.luaPath": "/path/to/lua5g",
  "lua5g.lspEnabled": true
}
```

## Architecture

### Smart Keywords
All Lua5g keywords (`class`, `extends`, `interface`, `enum`, `try`, `match`) are **contextual** — they work as keywords in statement position but as regular names after `.` and `:`:

```lua
class Foo end           -- keyword
t.class = 1             -- field name ✅
obj:match("pattern")    -- method call ✅
local match = string.match  -- variable ✅
```

### Keyword Rules
| Position | Behavior | Example |
|----------|----------|---------|
| Statement start | Keyword | `class Foo ... end` |
| After `.` | Field name | `t.class`, `s:match()` |
| After `:` | Method name | `obj:extends()` |
| Variable name | Name | `local class = 42` |

### File Types
| Extension | Purpose |
|-----------|---------|
| `.lua` | Lua5g source code |
| `.lua5g` | Lua5g source (explicit) |
| `.d.lua` | Type declarations (for C bindings) |
| `.luac` | Compiled bytecode |
| `.ldb` | Debug symbols |

## License

Same as Lua 5.5 — MIT License. See `doc/readme.html` for original Lua copyright.
