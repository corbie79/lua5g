# Built-in Tools & Libraries

## mathx — High Performance Math

```lua
local mathx = require("mathx")
```

### Native Float Arrays

```lua
local a = mathx.farray(1000000)     -- C-backed contiguous memory
a:fill_range(1.0, 1.0)              -- [1, 2, 3, ..., 1M]
a:map_sqrt()                        -- in-place sqrt (10x faster)
local s = a:sum()                   -- sum (10x faster)
local d = a:dot(b)                  -- dot product (15x faster)
b:saxpy(2.5, a)                     -- y = 2.5*x + y (20x faster)
a:set(i, val)                       -- set element
local v = a:get(i)                  -- get element
local n = a:len()                   -- length
```

### Batch Operations

```lua
mathx.sum(table, n)                 -- sum table elements
mathx.dot(a, b, n)                  -- dot product
mathx.map_sqrt(table, n)            -- in-place sqrt
mathx.saxpy(alpha, x, y, n)         -- BLAS saxpy
mathx.matmul(A, B, m, k, n)         -- matrix multiply
mathx.normalize(table, n, stride)   -- normalize vectors
```

## trace — Error Tracing

```lua
local trace = require("trace")
```

### Modes

| Mode | Output | Use Case |
|------|--------|----------|
| `"full"` | Variable names + values | Development |
| `"safe"` | Trace ID only (logs full to server) | Production API |
| `"encoded"` | Obfuscated tokens (needs .ldb to decode) | Live game |

```lua
trace.enable("full")
local ok, err = trace.pcall(function()
  error("something broke")
end)
print(err)
-- ERROR [a3f8b2c1] script.lua:42: something broke
--   script.lua:42 in function 'update'
--     player = {id=123, name="Kim", hp=0}
--     damage = 999
--     shield = nil
--   Trace ID: a3f8b2c1
```

### Encoded Mode (Live Service)

```lua
trace.enable("encoded")
-- Output: E[df2a] V[717a:5832:3] V[fba9:d290:4]
-- Variables are hashed — need .ldb to decode
```

### Custom Logger

```lua
trace.setlogger(function(full_trace, trace_id)
  send_to_server(full_trace)  -- server gets full info
end)
trace.enable("safe")
-- Client only sees: "Internal error [a3f8b2c1]"
```

## profile — Profiler

```lua
local profile = require("profile")

profile.start()
-- ... run code ...
profile.stop()
profile.report(10)    -- top 10 functions by time
profile.reset()       -- clear data
local data = profile.data()  -- get as Lua table
```

Output:
```
=== Profile Report ===
Total time: 0.2470 sec, 3 functions tracked

Function                          Calls   Total(s)        % Source
--------------------------------------------------------------------------
work                                  1   0.246955   100.0% script.lua:4
fib                             2189100   0.246930   100.0% script.lua:3
```

## test — Test Framework

```lua
local test = require("test")

test.describe("Calculator", function()
  test.it("should add numbers", function()
    test.expect(1 + 1).toBe(2)
    test.expect(3 * 4).toBe(12)
  end)

  test.it("should check types", function()
    test.expect("hello").toBeType("string")
    test.expect(42).toBeType("number")
    test.expect(nil).toBeNil()
    test.expect(true).toBeTruthy()
    test.expect(false).toBeFalsy()
  end)

  test.it("should find substrings", function()
    test.expect("hello world").toContain("world")
  end)
end)

test.summary()
```

### Assertions

| Method | Description |
|--------|-------------|
| `.toBe(expected)` | Strict equality |
| `.toEqual(expected)` | Same as toBe |
| `.toBeNil()` | Value is nil |
| `.toBeType(typename)` | Check type() result |
| `.toContain(substring)` | String contains |
| `.toBeTruthy()` | Not nil and not false |
| `.toBeFalsy()` | nil or false |

## ldb — Debug Symbols

```lua
local ldb = require("ldb")

-- Generate .ldb symbol file
ldb.generate("app.ldb", loadfile("app.lua"))

-- Read symbol info
local info = ldb.info("app.ldb")
print(info.version)
```

### Workflow

```bash
# Build time: generate symbols
lua5g -e 'require("ldb").generate("app.ldb", loadfile("app.lua"))'

# Release: strip bytecode
luac -s -o app.luac app.lua

# Deploy: app.luac (small) + app.ldb (for crash analysis)
```

## luac Options

```bash
luac -l script.lua           # disassemble
luac -l -l script.lua        # disassemble + locals with types
luac -t -o out.luac in.lua   # strip type checks (TYPECHECK → NOP)
luac -s -o out.luac in.lua   # full strip (all debug + types)
luac -p script.lua           # parse only (syntax check)
```

## lua5g CLI Options

```bash
lua5g script.lua             # run script
lua5g -e 'print(42)'        # run string
lua5g -i                     # interactive REPL
lua5g -d types.d.lua app.lua # load declarations before script
lua5g -l mod                 # require module before script
lua5g -v                     # show version
```
