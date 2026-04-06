# Modern Syntax Extensions

## Lambda Expressions

```lua
local double = |x| x * 2
local add = |x, y| x + y
local greet = |name| f"Hello {name}!"

-- With type annotations
local typed = |x: number, y: number| x + y

-- In higher-order functions
table.sort(items, |a, b| a.score > b.score)
local mapped = map(list, |x| x * 2)
```

Compiles to: `function(x) return x * 2 end`

## String Interpolation

```lua
local name = "World"
print(f"Hello {name}!")           -- Hello World!
print(f"1 + 2 = {result}")       -- 1 + 2 = 3
print(f"Score: {player.score}")   -- Score: 42
```

Supports variable names inside `{...}`. Compiles to `OP_CONCAT`.

## Nullable Chaining

```lua
local name = user?.profile?.name     -- nil if any part is nil
local count = list?.length           -- nil if list is nil
```

Compiles to `TEST + JMP + GETFIELD` (short-circuit evaluation).

## Table Destructuring

```lua
local {x, y, z} = get_position()
local {name, age} = user_data
local {width, height} = config.window
```

Compiles to: `local __tmp = expr; local x = __tmp.x; local y = __tmp.y`

## Pattern Matching

```lua
match status_code
  case 200 then handle_ok()
  case 404 then handle_not_found()
  case 500 then handle_error()
  case _ then handle_unknown()    -- default
end
```

Compiles to `if/elseif/else` chain using `OP_EQ`.

## Try / Except / Finally

```lua
try
  local data = parse(input)
  process(data)
except err then
  log_error(err)
  send_alert(err)
finally
  cleanup()  -- always executes
end
```

- `except` block runs only on error
- `finally` block always runs
- `err` variable is scoped to except block
- Compiles to `pcall` wrapper (zero overhead vs manual pcall)

## Import

```lua
import "math"                  -- local math = require("math")
import json from "dkjson"      -- local json = require("dkjson")
```

`import` is a contextual keyword (not reserved — can still be used as variable name).

## Async / Await

```lua
local result = async_run(function()
  local data = fetch_data()
  await()  -- yield point
  return process(data)
end)

-- async() wraps a function as coroutine
local fetch = async(function(url)
  -- async logic here
  await()
  return data
end)
```

Built on Lua coroutines. Zero new VM overhead.

## Keyword Rules

All Lua5g keywords are **contextual** — they behave as keywords only at the start of a statement. After `.` or `:`, they become regular names:

```lua
class Foo end              -- keyword ✅
t.class = 1                -- field name ✅
obj:match("pattern")       -- method call ✅
local match = string.match -- variable name ✅
```

This ensures full compatibility with existing Lua code that uses these words as identifiers.
