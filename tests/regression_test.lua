-- Comprehensive regression test suite for dala
-- Tests all added features: types, classes, access control, JIT

local pass, fail = 0, 0

local function test(name, fn)
  local ok, err = pcall(fn)
  if ok then
    pass = pass + 1
  else
    fail = fail + 1
    print("FAIL: " .. name .. " - " .. tostring(err))
  end
end

local function expect_error(name, fn, pattern)
  local ok, err = pcall(fn)
  if not ok and (pattern == nil or string.find(tostring(err), pattern)) then
    pass = pass + 1
  else
    fail = fail + 1
    if ok then
      print("FAIL: " .. name .. " - expected error, got success")
    else
      print("FAIL: " .. name .. " - wrong error: " .. tostring(err))
    end
  end
end

print("=== Dala Regression Tests ===\n")

-- ============================================================
-- 1. Standard Lua still works
-- ============================================================
test("standard: arithmetic", function()
  assert(1 + 2 == 3)
  assert(10 / 3 == 10/3)
  assert(2 ^ 10 == 1024)
end)

test("standard: strings", function()
  assert("hello" .. " " .. "world" == "hello world")
  assert(#"abc" == 3)
  assert(string.sub("hello", 1, 3) == "hel")
end)

test("standard: tables", function()
  local t = {1, 2, 3, a = "x"}
  assert(#t == 3)
  assert(t.a == "x")
  t[4] = 4
  assert(#t == 4)
end)

test("standard: functions", function()
  local function f(x) return x * 2 end
  assert(f(21) == 42)
end)

test("standard: closures", function()
  local function counter()
    local n = 0
    return function() n = n + 1; return n end
  end
  local c = counter()
  assert(c() == 1 and c() == 2 and c() == 3)
end)

test("standard: coroutines", function()
  local co = coroutine.create(function()
    coroutine.yield(1)
    coroutine.yield(2)
    return 3
  end)
  local _, v1 = coroutine.resume(co)
  local _, v2 = coroutine.resume(co)
  local _, v3 = coroutine.resume(co)
  assert(v1 == 1 and v2 == 2 and v3 == 3)
end)

test("standard: type function", function()
  assert(type(42) == "number")
  assert(type("hi") == "string")
  assert(type(true) == "boolean")
  assert(type({}) == "table")
  assert(type(nil) == "nil")
  assert(type(print) == "function")
end)

test("standard: pcall", function()
  local ok, err = pcall(error, "test")
  assert(not ok)
  assert(string.find(err, "test"))
end)

-- ============================================================
-- 2. Type annotations
-- ============================================================
test("type: number annotation", function()
  local x: number = 42
  assert(x == 42)
end)

test("type: string annotation", function()
  local x: string = "hello"
  assert(x == "hello")
end)

test("type: boolean annotation", function()
  local x: boolean = true
  assert(x == true)
end)

test("type: table annotation", function()
  local x: table = {1, 2}
  assert(#x == 2)
end)

test("type: function annotation", function()
  local x: function = function() return 1 end
  assert(x() == 1)
end)

test("type: any allows all", function()
  local x: any = 42
  x = "hello"
  assert(x == "hello")
end)

test("type: unknown allows all", function()
  local x: unknown = 42
  assert(x == 42)
end)

test("type: nullable allows nil", function()
  local x: number? = nil
  assert(x == nil)
end)

expect_error("type: number mismatch", function()
  local x: number = "bad"
end, "type error")

expect_error("type: string mismatch", function()
  local x: string = 42
end, "type error")

expect_error("type: boolean mismatch", function()
  local x: boolean = 42
end, "type error")

test("type: function params", function()
  local function add(a: number, b: number): number
    return a + b
  end
  assert(add(3, 4) == 7)
end)

test("type: type alias", function()
  type Point = {x: number, y: number}
  -- just parses, no error
end)

-- ============================================================
-- 3. Class declarations
-- ============================================================
test("class: basic", function()
  class TestAnimal
    function new(self, name)
      local inst = setmetatable({}, self)
      inst.name = name
      return inst
    end
    function speak(self)
      return self.name .. " speaks"
    end
  end
  local a = TestAnimal:new("Cat")
  assert(a:speak() == "Cat speaks")
end)

test("class: inheritance", function()
  class TestBase
    function new(self, x)
      local inst = setmetatable({}, self)
      inst.x = x
      return inst
    end
    function getX(self) return self.x end
  end
  class TestChild extends TestBase
    function getX2(self) return self.x * 2 end
  end
  local c = TestChild:new(21)
  assert(c:getX() == 21)   -- inherited
  assert(c:getX2() == 42)  -- own method
end)

test("class: multi-level inheritance", function()
  class TA function new(self) local i=setmetatable({},self); i.a=1; return i end end
  class TB extends TA function b(self) return 2 end end
  class TC extends TB function c(self) return 3 end end
  local obj = TC:new()
  assert(obj.a == 1)
  assert(obj:b() == 2)
  assert(obj:c() == 3)
end)

test("class: with field types", function()
  class TypedClass
    name: string
    age: number
    function new(self, n, a)
      local inst = setmetatable({}, self)
      inst.name = n
      inst.age = a
      return inst
    end
  end
  local t = TypedClass:new("test", 25)
  assert(t.name == "test")
  assert(t.age == 25)
end)

-- ============================================================
-- 4. Access control
-- ============================================================
test("access: readonly", function()
  class ROTest
    readonly x: number
    function new(self, val)
      local inst = setmetatable({}, self)
      inst.x = val
      return inst
    end
  end
  local r = ROTest:new(42)
  assert(r.x == 42)
end)

test("access: getter/setter", function()
  class GSTest
    private _val: number
    property val: number
      get(self) return self._val end
      set(self, v) self._val = v end
    end
    function new(self, v)
      local inst = setmetatable({}, self)
      inst._val = v
      return inst
    end
  end
  local g = GSTest:new(10)
  assert(g.val == 10)
  g.val = 20
  assert(g.val == 20)
end)

-- ============================================================
-- 5. JIT
-- ============================================================
test("jit: status", function()
  local arch = __jit_status()
  assert(arch == "x86-64" or arch == "arm64" or arch == "none")
end)

test("jit: manual compile", function()
  local function sum(n)
    local s = 0
    for i = 1, n do s = s + i end
    return s
  end
  assert(sum(100) == 5050)
  local ok = __jit_compile(sum)
  assert(ok)
  assert(sum(100) == 5050)
end)

test("jit: correctness edge cases", function()
  local function sum(n)
    local s = 0
    for i = 1, n do s = s + i end
    return s
  end
  __jit_compile(sum)
  assert(sum(0) == 0)
  assert(sum(1) == 1)
  assert(sum(2) == 3)
  assert(sum(10) == 55)
  assert(sum(1000) == 500500)
end)

test("jit: auto detection", function()
  local function auto_sum(n)
    local s = 0
    for i = 1, n do s = s + i end
    return s
  end
  -- warmup past threshold
  for w = 1, 120 do auto_sum(10) end
  -- should be JIT compiled now, verify correctness
  assert(auto_sum(100) == 5050)
  assert(auto_sum(1000) == 500500)
end)

-- ============================================================
-- Summary
-- ============================================================
print(string.format("\nResults: %d passed, %d failed, %d total",
      pass, fail, pass + fail))
if fail == 0 then
  print("ALL TESTS PASSED!")
else
  print("SOME TESTS FAILED!")
  os.exit(1)
end
