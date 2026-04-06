# Modern Syntax

---

## Lambda

```lua
local double = |x| x * 2
local add = |x, y| x + y
table.sort(items, |a, b| a.score > b.score)
```

Compiles to `function(x) return x * 2 end`.

---

## String Interpolation

```lua
local name = "World"
print(f"Hello {name}!")
print(f"Score: {player.score}")
```

Supports variable names inside `{...}`. Compiles to `OP_CONCAT`.

---

## Nullable Chaining

```lua
local name = user?.profile?.name   -- nil if any part is nil
```

Compiles to `TEST + JMP + GETFIELD` (short-circuit).

---

## Table Destructuring

```lua
local {x, y, z} = get_position()
local {name, age} = user_data
```

---

## Pattern Matching

```lua
match status_code
  case 200 then handle_ok()
  case 404 then handle_not_found()
  case _ then handle_unknown()
end
```

---

## Try / Except / Finally

```lua
try
  parse(input)
except err then
  log_error(err)
finally
  cleanup()
end
```

- `except` runs only on error
- `finally` always runs
- Compiles to `pcall` (zero overhead)

---

## Import

```lua
import "math"                -- local math = require("math")
import json from "dkjson"    -- local json = require("dkjson")
```

---

## Async / Await

```lua
local result = async_run(function()
  await()
  return process(data)
end)
```

Built on Lua coroutines.

---

## Contextual Keywords

All Lua5g keywords behave as regular names after `.` or `:`:

```lua
class Foo end            -- keyword
t.class = 1              -- field ✅
obj:match("pattern")     -- method ✅
local match = string.match  -- variable ✅
```
