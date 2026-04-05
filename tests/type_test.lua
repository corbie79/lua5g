-- Test file for type annotations
-- Type annotations are parsed but not enforced at runtime (gradual typing)

print("=== Type Annotation Tests ===")

-- Basic type annotations on local variables
local x: number = 42
local y: string = "hello"
local z: boolean = true
local t: table = {1, 2, 3}
local f: function = function() return 1 end
local n: nil = nil
assert(x == 42)
assert(y == "hello")
assert(z == true)
assert(#t == 3)
assert(f() == 1)
assert(n == nil)
print("PASS: basic local type annotations")

-- Optional (nullable) types
local a: number? = nil
local b: string? = "test"
assert(a == nil)
assert(b == "test")
print("PASS: nullable type annotations")

-- Multiple variables with types
local m: number, k: string = 10, "world"
assert(m == 10)
assert(k == "world")
print("PASS: multiple typed local variables")

-- Function parameter type annotations
local function add(a: number, b: number): number
  return a + b
end
assert(add(3, 4) == 7)
print("PASS: function parameter and return type annotations")

-- Function with mixed typed/untyped params
local function greet(name: string, times)
  local result = ""
  for i = 1, times do
    result = result .. "Hello " .. name .. "! "
  end
  return result
end
assert(string.find(greet("World", 2), "Hello World!"))
print("PASS: mixed typed/untyped parameters")

-- Method-style function with types
local obj = {}
function obj.calc(self, x: number, y: number): number
  return x * y
end
assert(obj.calc(nil, 3, 4) == 12)
print("PASS: method-style function type annotations")

-- Type annotations don't enforce types (gradual typing)
local typed_var: number = "actually a string"  -- no error!
assert(typed_var == "actually a string")
print("PASS: type annotations are not enforced (gradual typing)")

-- Type aliases
type Point = {x: number, y: number}
type Callback = (number, string)
print("PASS: type aliases parsed")

-- Union types
local val: number | string = 42
val = "hello"
assert(val == "hello")
print("PASS: union type annotations")

print("")
print("=== All type annotation tests passed! ===")
