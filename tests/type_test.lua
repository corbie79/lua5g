-- Test file for type annotations
-- Type annotations are now checked at runtime for basic types

print("=== Type Annotation Tests ===")

-- Basic type annotations on local variables (runtime checked)
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
print("PASS: basic local type annotations with runtime checks")

-- Type mismatch detection
local ok, err

ok, err = pcall(function()
  local x: number = "wrong"
end)
assert(not ok)
assert(string.find(err, "type error"))
assert(string.find(err, "'number' expected"))
assert(string.find(err, "got 'string'"))
print("PASS: number type mismatch detected")

ok, err = pcall(function()
  local x: string = 42
end)
assert(not ok)
assert(string.find(err, "'string' expected"))
assert(string.find(err, "got 'number'"))
print("PASS: string type mismatch detected")

ok, err = pcall(function()
  local x: boolean = "yes"
end)
assert(not ok)
assert(string.find(err, "'boolean' expected"))
print("PASS: boolean type mismatch detected")

ok, err = pcall(function()
  local x: table = 42
end)
assert(not ok)
assert(string.find(err, "'table' expected"))
print("PASS: table type mismatch detected")

ok, err = pcall(function()
  local x: function = 42
end)
assert(not ok)
assert(string.find(err, "'function' expected"))
print("PASS: function type mismatch detected")

-- Error messages include variable name
ok, err = pcall(function()
  local myVar: number = "oops"
end)
assert(string.find(err, "variable 'myVar'"))
print("PASS: error messages include variable name")

-- 'any' type allows everything (no checking)
local a1: any = 42
local a2: any = "hello"
local a3: any = true
local a4: any = {}
local a5: any = nil
assert(a1 == 42)
assert(a2 == "hello")
print("PASS: 'any' type allows all values")

-- 'unknown' type accepts all values
local u1: unknown = 42
local u2: unknown = "hello"
local u3: unknown = true
local u4: unknown = {}
assert(u1 == 42)
assert(u2 == "hello")
print("PASS: 'unknown' type accepts all values")

-- Optional (nullable) types - nil is always allowed
local nx: number? = nil
assert(nx == nil)
local ny: string? = nil
assert(ny == nil)
local nz: number? = 42
assert(nz == 42)
print("PASS: nullable type annotations allow nil")

-- Multiple variables with types
local m: number, k: string = 10, "world"
assert(m == 10)
assert(k == "world")
print("PASS: multiple typed local variables")

-- Type mismatch in multiple declarations
ok, err = pcall(function()
  local a: number, b: string = "wrong", 42
end)
assert(not ok)
assert(string.find(err, "type error"))
print("PASS: type mismatch detected in multiple declarations")

-- Function parameter type annotations (parsed, no runtime check on params yet)
local function add(a: number, b: number): number
  return a + b
end
assert(add(3, 4) == 7)
print("PASS: function parameter and return type annotations")

-- Untyped variables still work normally
local plain = 42
plain = "now a string"
assert(plain == "now a string")
print("PASS: untyped variables work normally")

-- Type aliases (parsed and discarded)
type Point = {x: number, y: number}
type Callback = (number, string)
print("PASS: type aliases parsed")

-- Union types (parsed, no runtime check for unions)
local val: number | string = 42
val = "hello"
assert(val == "hello")
print("PASS: union type annotations parsed")

-- type() function still works
assert(type(42) == "number")
assert(type("hi") == "string")
assert(type(true) == "boolean")
assert(type({}) == "table")
print("PASS: type() function works normally")

print("")
print("=== All type annotation tests passed! ===")
