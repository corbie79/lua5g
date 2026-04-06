--[[
  LuaJIT-compatible benchmark (no Lua 5.5 features).
  Run with: luajit bench_compat.lua
            ./src/lua bench_compat.lua
]]
local clock = os.clock
local fmt = string.format
local sqrt = math.sqrt
local N = 5000000
local RUNS = 3
local results = {}

local function bench(name, fn)
  fn()
  local best = math.huge
  for r = 1, RUNS do
    local t = clock()
    fn()
    local elapsed = clock() - t
    if elapsed < best then best = elapsed end
  end
  results[#results + 1] = {name = name, time = best}
end

print(fmt("%-42s %12s", "Benchmark", "Time (sec)"))
print(string.rep("-", 56))

-- 1. Integer sum
bench("1. Integer sum 1..N", function()
  local sum = 0
  for i = 1, N do sum = sum + i end
end)

-- 2. Fibonacci call
local function fib(n)
  local a, b = 0, 1
  for i = 1, n do a, b = b, a + b end
  return a
end
bench("2. Fibonacci(20) x N", function()
  for i = 1, N do fib(20) end
end)

-- 3. Float math
bench("3. Float N-body gravity", function()
  local fx, fy = 0.0, 0.0
  for i = 0, N - 1 do
    local dx = 1.0 + i * 0.001
    local dy = 1.0 + i * 0.001
    local d2 = dx * dx + dy * dy + 0.01
    local inv = 10.0 / (d2 * sqrt(d2))
    fx = fx + dx * inv
    fy = fy + dy * inv
  end
end)

-- 4. Empty function call
local function noop() end
bench("4. Empty func call x N", function()
  for i = 1, N do noop() end
end)

-- 5. Vec2 normalize
bench("5. Vec2 normalize x 1M", function()
  local x, y = 1.0, 2.0
  for i = 1, 1000000 do
    local nx = x * 0.99 + y * 0.01
    local ny = y * 0.99 - x * 0.01
    local len = sqrt(nx * nx + ny * ny)
    x = nx / len
    y = ny / len
  end
end)

-- 6. Table creation
bench("6. Table create x 1M", function()
  for i = 1, 1000000 do
    local t = {x = 1, y = 2, z = 3}
  end
end)

-- 7. Mat4 multiply
local function mat4_mul(a, b)
  local c = {}
  for i = 0, 3 do
    for j = 0, 3 do
      local s = 0
      for k = 0, 3 do
        s = s + a[i*4+k+1] * b[k*4+j+1]
      end
      c[i*4+j+1] = s
    end
  end
  return c
end
local A = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
local B = {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1}
bench("7. Mat4 multiply x 200K", function()
  for i = 1, 200000 do mat4_mul(A, B) end
end)

-- 8. String concat
bench("8. String concat x 100K", function()
  local s = ""
  for i = 1, 100000 do
    s = s .. "x"
  end
end)

-- Print
print(string.rep("-", 56))
for _, r in ipairs(results) do
  print(fmt("%-42s %12.4f", r.name, r.time))
end
