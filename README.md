<p align="center"><img src="assets/dala-icon.svg" width="200" alt="Dala"></p>

# Dala

> **Lua 5.5 fork with type system, classes, JIT compiler, and modern language features.**

Dala extends Lua 5.5.0 with TypeScript/Luau-inspired type annotations, a full class system with inheritance and access control, a lightweight JIT compiler, and modern syntax — while maintaining full backward compatibility with standard Lua.

---

## Performance

**JIT Compiler** — 50M integer sum benchmark:

- **Dala JIT (x86-64)** — `0.037s` / `1,340 Mops` — matches LuaJIT
- **LuaJIT (JIT ON)** — `0.035s` / `1,412 Mops`
- **LuaJIT (JIT OFF)** — `0.203s` / `246 Mops`
- **Dala interpreter** — `0.229s` / `219 Mops`

**Native Arrays** (`mathx.farray`) vs Lua tables:

- `sum` → **9.7x** faster
- `sqrt` → **9.8x** faster
- `dot` → **15.3x** faster
- `saxpy` → **20.1x** faster

---

## New Keywords

> All keywords are **contextual** — they work as identifiers after `.` and `:`, so `t.class = 1` and `s:match("pattern")` work fine.

### Class & OOP

- **`class`** — Declare a class → `class Animal ... end`
- **`extends`** — Inherit from parent → `class Dog extends Animal`
- **`implements`** — Implement interface → `class Foo implements Bar`
- **`interface`** — Method contract → `interface Drawable ... end`
- **`enum`** — Named constants → `enum Color RED GREEN BLUE end`
- **`override`** — Required to override parent method
- **`super`** — Call parent method → `super.speak(self)`
- **`abstract`** — Method without body (must be overridden)
- **`static`** — Class method without self
- **`operator`** — Metamethod shorthand → `operator + (a, b) ... end`
- **`property`** — Getter/setter → `property hp get(self)... set(self,v)... end`
- **`declare`** — C binding type declaration → `declare class Vec2 ... end`

### Access Control

- **`public`** — Accessible everywhere (default)
- **`private`** — Class methods only
- **`protected`** — Class + subclass methods
- **`readonly`** — Write once, then immutable

### Control Flow

- **`try`** / **`except`** / **`finally`** — Exception handling
- **`match`** / **`case`** — Pattern matching

### Other

- **`import`** — Module import → `import "math"`
- **`type`** — Type alias → `type Point = {x: number}`

---

## Quick Start

```bash
# Build
make linux

# Run
./src/lua script.lua

# REPL with tab-completion
make linux MYCFLAGS="-DLUA_USE_READLINE" MYLIBS="-lreadline"
./src/lua
```

### Type Checking Modes

```bash
dala script.lua            # default: types optional (gradual)
dala --strict script.lua   # all locals must have types
dala --legacy script.lua   # type annotations ignored
```

---

## Features at a Glance

```lua
-- Type annotations
local name: string = "Dala"
local count: number = 42

-- Classes
class Animal
  private _health: number
  readonly species: string

  function new(self, name: string)
    local inst = setmetatable({}, self)
    inst.name = name
    inst._health = 100
    return inst
  end
end

class Dog extends Animal
  override function speak(self): string
    return super.speak(self) .. " and barks"
  end
end

-- RTTI
print(instanceof(d, Animal))  -- true
print(classname(d))           -- "Dog"

-- Pattern matching
match status
  case 200 then print("OK")
  case 404 then print("Not Found")
  case _ then print("Unknown")
end

-- Exception handling
try
  dangerous()
except err then
  print("caught:", err)
finally
  cleanup()
end

-- Lambda, f-string, nullable chaining
local double = |x| x * 2
print(f"Result: {double(21)}")
local name = user?.profile?.name
```

---

## Documentation

Detailed documentation is available in the `docs/` directory:

- **[Type System](docs/type-system.md)** — Annotations, checking, generics, stripping
- **[Class System](docs/classes.md)** — OOP, inheritance, access control, interfaces, enums
- **[JIT Compiler](docs/jit.md)** — Architecture, register promotion, benchmarks
- **[C Binding API](docs/c-api.md)** — Creating classes from C, declaration files
- **[Modern Syntax](docs/syntax.md)** — Lambda, f-string, `?.`, destructuring, match, try
- **[Tools & Libraries](docs/tools.md)** — mathx, trace, profiler, test framework, ldb

---

## VSCode Extension

Full IDE support in `vscode-dala/`:

- **Syntax highlighting** — All Dala keywords, types, modifiers
- **12 code snippets** — class, try, match, lambda, enum, etc.
- **LSP server** — Autocomplete, hover info
- **DAP debugger** — Breakpoints, step, variable inspection
- **Tab completion** — Keywords + globals + table fields in REPL

```bash
cp -r vscode-dala ~/.vscode/extensions/
```

---

## JIT Architectures

- **x86-64** — Production ready (int + float, SSE2)
- **x86-32** — Tested (int + float, SSE2)
- **ARMv7** — QEMU verified (int + float, VFP)
- **ARM64** — Emitter complete (needs hardware test)

---

## Cross-compilation

```bash
# ARMv7
arm-linux-gnueabihf-gcc -static -DLUA_USE_LINUX src/*.c -o dala_arm -lm

# x86 32-bit
gcc -m32 -DLUA_USE_LINUX src/*.c -o dala_x86 -lm
```

---

## License

Same as Lua 5.5 — MIT License. See `doc/readme.html`.
