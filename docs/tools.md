# Tools & Libraries

---

## mathx — High Performance Math

```lua
local mathx = require("mathx")
```

### Native Float Arrays

C-backed contiguous memory — no Lua table overhead:

```lua
local a = mathx.farray(1000000)
a:fill_range(1.0, 1.0)     -- [1, 2, ..., 1M]
a:map_sqrt()                -- in-place, 10x faster
local s = a:sum()           -- 10x faster
local d = a:dot(b)          -- 15x faster
b:saxpy(2.5, a)             -- 20x faster
```

### Batch Operations

```lua
mathx.sum(table, n)
mathx.dot(a, b, n)
mathx.map_sqrt(table, n)
mathx.saxpy(alpha, x, y, n)
mathx.matmul(A, B, m, k, n)
```

---

## trace — Error Tracing

```lua
local trace = require("trace")
```

### Three Modes

- **`trace.enable("full")`** — Shows variable names + values (dev)
- **`trace.enable("encoded")`** — Obfuscated tokens, needs `.ldb` to decode (live)
- **`trace.enable("safe")`** — Trace ID only, logs full to server (production)

```lua
trace.enable("full")
local ok, err = trace.pcall(function()
  error("crash")
end)
-- ERROR [a3f8b2c1] script.lua:42: crash
--   player = {id=123, hp=0}
--   shield = nil
--   Trace ID: a3f8b2c1
```

### Custom Logger

```lua
trace.setlogger(function(full_trace, id)
  send_to_server(full_trace)
end)
trace.enable("safe")
-- Client sees: "Internal error [a3f8b2c1]"
```

---

## profile — Profiler

```lua
local profile = require("profile")

profile.start()
run_code()
profile.stop()
profile.report(10)   -- top 10 functions
profile.reset()
```

Output:
```
fib    2189100  0.246930  100.0%  script.lua:3
```

---

## test — Test Framework

```lua
local test = require("test")

test.describe("Calculator", function()
  test.it("should add", function()
    test.expect(1 + 1).toBe(2)
  end)
  test.it("should check types", function()
    test.expect("hello").toBeType("string")
    test.expect(nil).toBeNil()
    test.expect("hello world").toContain("world")
    test.expect(true).toBeTruthy()
  end)
end)

test.summary()
```

---

## ldb — Debug Symbols

```lua
local ldb = require("ldb")
ldb.generate("app.ldb", loadfile("app.lua"))
```

Workflow:

1. Build time: `ldb.generate("app.ldb", loadfile("app.lua"))`
2. Release: `luac -s -o app.luac app.lua`
3. Deploy: `app.luac` (small) + `app.ldb` (crash analysis)

---

## CLI Options

### lua5g

```bash
lua5g script.lua              # run script
lua5g -e 'print(42)'          # run string
lua5g -i                       # interactive REPL
lua5g -d types.d.lua app.lua   # load declarations first
lua5g --strict script.lua      # require types on all locals
lua5g --legacy script.lua      # ignore type annotations
```

### luac

```bash
luac -l script.lua             # disassemble
luac -l -l script.lua          # disassemble + locals with types
luac -t -o out.luac in.lua     # strip type checks
luac -s -o out.luac in.lua     # strip everything
```

---

## Document Generator

```bash
lua5g tools/docgen.lua api.d.lua > api.md
```

Parses `declare class`, `interface`, `enum` from `.d.lua` files and generates markdown.
