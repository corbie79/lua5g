--[[
  Comprehensive Performance Benchmark
  Compares: Lua 5.5g (interpreted), C call, typed vs untyped
  Run separately with LuaJIT for comparison.
]]

local clock = os.clock
local fmt = string.format

-- Try to load C bench module
local cbench_ok, cbench = pcall(require, "cbench")
if not cbench_ok then
  print("WARNING: cbench module not found, C benchmarks will be skipped")
  print("  Build with: gcc -shared -o cbench.so cbench.c -I../src")
  cbench = nil
end

local N = 5000000   -- iteration count for most tests
local RUNS = 3      -- number of runs for averaging

local results = {}

local function bench(name, fn)
  -- warmup
  fn()
  -- measure
  local best = math.huge
  for r = 1, RUNS do
    local t = clock()
    fn()
    local elapsed = clock() - t
    if elapsed < best then best = elapsed end
  end
  results[#results + 1] = {name = name, time = best}
  return best
end

print("=" .. string.rep("=", 72))
print("  Lua5g Performance Benchmark")
print("  Iterations: " .. N)
print("=" .. string.rep("=", 72))

-- ============================================================
-- 1. Integer Arithmetic
-- ============================================================
print("\n--- 1. Integer Sum (1..N) ---")

bench("Lua sum (untyped)", function()
  local sum = 0
  for i = 1, N do sum = sum + i end
  return sum
end)

bench("Lua sum (typed)", function()
  local sum: number = 0
  for i = 1, N do sum = sum + i end
  return sum
end)

if cbench then
  bench("C sum (single call)", function()
    return cbench.sum(N)
  end)
end

-- ============================================================
-- 2. Fibonacci (iterative)
-- ============================================================
print("\n--- 2. Fibonacci Iterative ---")

local function lua_fib(n)
  local a, b = 0, 1
  for i = 1, n do
    a, b = b, a + b
  end
  return a
end

bench("Lua fib (call per iter)", function()
  for i = 1, N do
    lua_fib(20)
  end
end)

if cbench then
  bench("C fib (call per iter)", function()
    for i = 1, N do
      cbench.fib(20)
    end
  end)
end

-- ============================================================
-- 3. Floating-point Heavy (N-body style)
-- ============================================================
print("\n--- 3. Float Math (N-body gravity) ---")

bench("Lua nbody", function()
  local x1, y1, x2, y2, mass = 0.0, 0.0, 1.0, 1.0, 10.0
  local fx, fy = 0.0, 0.0
  for i = 0, N - 1 do
    local dx = x2 - x1 + i * 0.001
    local dy = y2 - y1 + i * 0.001
    local dist2 = dx * dx + dy * dy + 0.01
    local inv = mass / (dist2 * math.sqrt(dist2))
    fx = fx + dx * inv
    fy = fy + dy * inv
  end
  return fx, fy
end)

if cbench then
  bench("C nbody (single call)", function()
    return cbench.nbody_step(0, 0, 1, 1, 10, N)
  end)
end

-- ============================================================
-- 4. Function Call Overhead
-- ============================================================
print("\n--- 4. Function Call Overhead ---")

local function noop() end

bench("Lua empty func call x N", function()
  for i = 1, N do noop() end
end)

if cbench then
  bench("C noop loop (single call)", function()
    cbench.noop_loop(N)
  end)
end

-- Lua->C call overhead
if cbench then
  bench("Lua->C call x N", function()
    for i = 1, N do cbench.fib(1) end
  end)
end

-- ============================================================
-- 5. Vec2 Operations (OOP)
-- ============================================================
print("\n--- 5. Vec2 OOP (normalize loop) ---")

local VN = 1000000

bench("Lua Vec2 (plain table)", function()
  local x, y = 1.0, 2.0
  for i = 1, VN do
    local nx = x * 0.99 + y * 0.01
    local ny = y * 0.99 - x * 0.01
    local len = math.sqrt(nx * nx + ny * ny)
    x = nx / len
    y = ny / len
  end
  return x, y
end)

-- Using class
class Vec2Bench
  function new(x, y)
    local self = setmetatable({}, Vec2Bench)
    self.x = x
    self.y = y
    return self
  end
  function normalize(self)
    local len = math.sqrt(self.x * self.x + self.y * self.y)
    self.x = self.x / len
    self.y = self.y / len
  end
end

bench("Lua Vec2 (class, no access ctrl)", function()
  local v = Vec2Bench.new(1.0, 2.0)
  for i = 1, VN do
    local nx = v.x * 0.99 + v.y * 0.01
    local ny = v.y * 0.99 - v.x * 0.01
    v.x = nx
    v.y = ny
    v:normalize()
  end
  return v.x, v.y
end)

if cbench then
  bench("C Vec2 ops (single call)", function()
    return cbench.vec2_ops(VN)
  end)
end

-- ============================================================
-- 6. Matrix 4x4 Multiply
-- ============================================================
print("\n--- 6. Matrix 4x4 Multiply ---")

local MN = 200000

local function lua_mat4_mul(a, b)
  local c = {}
  for i = 0, 3 do
    for j = 0, 3 do
      local s = 0
      for k = 0, 3 do
        s = s + a[i * 4 + k + 1] * b[k * 4 + j + 1]
      end
      c[i * 4 + j + 1] = s
    end
  end
  return c
end

local mat_a = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16}
local mat_b = {16,15,14,13, 12,11,10,9, 8,7,6,5, 4,3,2,1}

bench("Lua mat4 multiply x " .. MN, function()
  local r
  for i = 1, MN do r = lua_mat4_mul(mat_a, mat_b) end
  return r
end)

if cbench then
  bench("C mat4 multiply x " .. MN, function()
    local r
    for i = 1, MN do r = cbench.mat4_mul(mat_a, mat_b) end
    return r
  end)
end

-- ============================================================
-- 7. Type Check Overhead
-- ============================================================
print("\n--- 7. Type Annotation Overhead ---")

bench("untyped local assign x N", function()
  for i = 1, N do
    local x = 42
  end
end)

bench("typed local assign x N", function()
  for i = 1, N do
    local x: number = 42
  end
end)

-- ============================================================
-- Print Results Table
-- ============================================================
print("\n" .. string.rep("=", 73))
print(fmt("  %-40s %12s", "Benchmark", "Time (sec)"))
print(string.rep("-", 73))
for _, r in ipairs(results) do
  print(fmt("  %-40s %12.4f", r.name, r.time))
end
print(string.rep("=", 73))
