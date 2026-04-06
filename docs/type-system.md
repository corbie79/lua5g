# Type System

Lua5g adds gradual type annotations to Lua. Types are checked at both compile time and runtime.

## Type Annotations

```lua
local x: number = 42
local y: string = "hello"
local z: boolean = true
local t: table = {1, 2, 3}
local f: function = function() return 1 end
```

## Special Types

| Type | Meaning |
|------|---------|
| `any` | No type checking (fully dynamic) |
| `unknown` | Accepts all values, tracked in debug info |
| `number?` | Nullable: number or nil |
| `A \| B` | Union type (parsed, not runtime-checked) |

## Compile-time Checking

Literal type mismatches are caught at load time:

```lua
local x: number = "hello"
-- ERROR: type error: 'number' expected for variable 'x', got 'string'
```

Unknown type names are also caught:

```lua
local x: FooBar = 42
-- ERROR: unknown type 'FooBar'
```

## Runtime Checking (OP_TYPECHECK)

When the value is not a literal (e.g., function return), runtime checking occurs via the `OP_TYPECHECK` bytecode instruction:

```lua
local function get_value() return "not a number" end
local x: number = get_value()  -- runtime error
```

### Type IDs (Fast Path)

Built-in types use numeric IDs for zero-strcmp overhead:

| ID | Type |
|----|------|
| 0 | number |
| 1 | string |
| 2 | boolean |
| 3 | table |
| 4 | function |
| 5 | nil |
| 6 | thread |
| 7 | userdata |
| 8 | any (no check) |
| 9 | unknown (no check) |
| 10 | class (metatable chain check) |

## Class Type Checking

When a variable is typed with a class name, the runtime verifies the value is an instance (via metatable chain):

```lua
class Animal ... end
class Dog extends Animal ... end

local a: Animal = Dog:new("Rex")  -- OK (Dog extends Animal)
local d: Dog = Animal:new("Cat")  -- ERROR (Animal is not Dog)
```

## Function Parameter Types

```lua
function add(a: number, b: number): number
  return a + b
end
```

Parameter types are parsed but not runtime-checked (annotations only). Return type is also annotation-only.

## Generic Types

```lua
local list: Array<number> = {1, 2, 3}
local map: Map<string, Array<number>> = {}
```

Generic types are parsed (including nested `<>`) but not enforced at runtime. They serve as documentation and tooling hints.

## Type Aliases

```lua
type Point = {x: number, y: number}
type Callback = (number, string)
```

Parsed and discarded. For documentation and LSP tooling.

## Stripping Types

```bash
luac -t -o release.luac script.lua   # OP_TYPECHECK → MOVE A A (NOP)
luac -s -o minimal.luac script.lua   # full strip + type info removed
```

In release builds, type annotations have zero runtime cost.

## Debug Info

Type annotations are stored in `LocVar.typename_` and visible in `luac -l -l`:

```
locals (2) for ...:
    0   x   number   3   8
    1   y   string   5   8
```
