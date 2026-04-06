# Type System

Lua5g adds gradual type annotations to Lua. Types are checked at both compile time and runtime.

---

## Type Annotations

```lua
local x: number = 42
local y: string = "hello"
local z: boolean = true
local t: table = {1, 2, 3}
local f: function = function() return 1 end
```

### Special Types

- **`any`** — No type checking (fully dynamic)
- **`unknown`** — Accepts all values, tracked in debug info
- **`number?`** — Nullable: number or nil
- **`A | B`** — Union type (parsed, not runtime-checked)

### Generic Types

```lua
local list: Array<number> = {1, 2, 3}
local map: Map<string, Array<number>> = {}
```

Generic types are parsed (including nested `<>`) but not enforced. They serve as documentation and tooling hints.

### Type Aliases

```lua
type Point = {x: number, y: number}
type Callback = (number, string)
```

---

## Compile-time Checking

Literal mismatches are caught at load time — no runtime cost:

```lua
local x: number = "hello"
-- ERROR: type error: 'number' expected for variable 'x', got 'string'
```

Unknown type names:

```lua
local x: FooBar = 42
-- ERROR: unknown type 'FooBar'
```

---

## Runtime Checking

When the value is dynamic (e.g., function return), checking happens via `OP_TYPECHECK`:

```lua
local function get() return "not a number" end
local x: number = get()  -- runtime error
```

Built-in types use numeric IDs for zero-strcmp overhead:

- `0` number, `1` string, `2` boolean, `3` table, `4` function
- `5` nil, `6` thread, `7` userdata
- `8` any (no check), `9` unknown (no check), `10` class (metatable chain)

---

## Class Type Checking

```lua
class Animal ... end
class Dog extends Animal ... end

local a: Animal = Dog:new("Rex")  -- OK (Dog extends Animal)
local d: Dog = Animal:new("Cat")  -- ERROR (Animal is not Dog)
```

---

## Function Types

```lua
function add(a: number, b: number): number
  return a + b
end
```

Parameter and return types are annotations (parsed, not enforced at runtime).

---

## Checking Modes

```bash
lua5g script.lua            # default: types optional
lua5g --strict script.lua   # all locals must have type annotations
lua5g --legacy script.lua   # type annotations completely ignored
```

---

## Stripping for Release

```bash
luac -t -o release.luac script.lua   # OP_TYPECHECK → NOP, keep debug names
luac -s -o minimal.luac script.lua   # strip everything
```

---

## Debug Info

Type annotations are stored in `LocVar.typename_` and visible in disassembly:

```
luac -l -l script.lua

locals (2):
    0   x   number   3   8
    1   y   string   5   8
```
