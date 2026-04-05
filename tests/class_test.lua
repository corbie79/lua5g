-- Test file for class declarations and inheritance

print("=== Class Declaration Tests ===")

-- Basic class with constructor and methods
class Animal
  name: string
  age: number

  function new(self, name, age)
    local instance = setmetatable({}, self)
    instance.name = name
    instance.age = age
    return instance
  end

  function speak(self)
    return self.name .. " makes a sound"
  end

  function getAge(self)
    return self.age
  end
end

local cat = Animal:new("Whiskers", 5)
assert(cat:speak() == "Whiskers makes a sound")
assert(cat:getAge() == 5)
assert(cat.name == "Whiskers")
print("PASS: basic class creation and methods")

-- Multiple instances are independent
local dog = Animal:new("Rex", 3)
assert(dog.name == "Rex")
assert(cat.name == "Whiskers")  -- cat unchanged
assert(dog ~= cat)
print("PASS: independent instances")

-- Class inheritance
class Dog extends Animal
  breed: string

  function new(self, name, age, breed)
    local instance = setmetatable({}, self)
    instance.name = name
    instance.age = age
    instance.breed = breed
    return instance
  end

  function speak(self)
    return self.name .. " barks"
  end

  function getBreed(self)
    return self.breed
  end
end

local rex = Dog:new("Rex", 3, "Shepherd")
assert(rex:speak() == "Rex barks")  -- overridden method
assert(rex:getBreed() == "Shepherd")  -- new method
assert(rex:getAge() == 3)  -- inherited method from Animal
assert(rex.name == "Rex")
print("PASS: class inheritance with method override")

-- Verify inheritance chain
local buddy = Dog:new("Buddy", 2, "Poodle")
assert(buddy:speak() == "Buddy barks")
assert(buddy:getAge() == 2)  -- inherited from Animal
assert(rex:speak() == "Rex barks")  -- rex unchanged
print("PASS: multiple inherited instances")

-- Multi-level inheritance
class GuideDog extends Dog
  function new(self, name, age, breed)
    local instance = setmetatable({}, self)
    instance.name = name
    instance.age = age
    instance.breed = breed
    instance.isGuide = true
    return instance
  end

  function describe(self)
    return self.name .. " is a guide " .. self.breed
  end
end

local guide = GuideDog:new("Lassie", 4, "Collie")
assert(guide:describe() == "Lassie is a guide Collie")
assert(guide:speak() == "Lassie barks")  -- inherited from Dog
assert(guide:getAge() == 4)  -- inherited from Animal
assert(guide:getBreed() == "Collie")  -- inherited from Dog
assert(guide.isGuide == true)
print("PASS: multi-level inheritance (3 levels)")

-- Class without extends (standalone)
class Counter
  function new(self, start)
    local instance = setmetatable({}, self)
    instance.count = start or 0
    return instance
  end

  function increment(self)
    self.count = self.count + 1
  end

  function getCount(self)
    return self.count
  end
end

local c = Counter:new(10)
c:increment()
c:increment()
c:increment()
assert(c:getCount() == 13)
print("PASS: standalone class (no extends)")

-- Class with type-annotated fields and methods
class Point
  x: number
  y: number

  function new(self, x: number, y: number): table
    local instance = setmetatable({}, self)
    instance.x = x
    instance.y = y
    return instance
  end

  function distanceTo(self, other: table): number
    local dx = self.x - other.x
    local dy = self.y - other.y
    return math.sqrt(dx * dx + dy * dy)
  end

  function toString(self): string
    return "(" .. self.x .. ", " .. self.y .. ")"
  end
end

local p1 = Point:new(0, 0)
local p2 = Point:new(3, 4)
assert(p1:distanceTo(p2) == 5.0)
assert(p1:toString() == "(0, 0)")
assert(p2:toString() == "(3, 4)")
print("PASS: class with type annotations on fields and methods")

-- Dot-call constructor style (alternative pattern)
class Vec2
  function new(x, y)
    local self = setmetatable({}, Vec2)
    self.x = x
    self.y = y
    return self
  end

  function add(self, other)
    return Vec2.new(self.x + other.x, self.y + other.y)
  end

  function len(self)
    return math.sqrt(self.x * self.x + self.y * self.y)
  end
end

local v1 = Vec2.new(1, 2)
local v2 = Vec2.new(3, 4)
local v3 = v1:add(v2)
assert(v3.x == 4)
assert(v3.y == 6)
assert(Vec2.new(3, 4):len() == 5.0)
print("PASS: dot-call constructor pattern")

-- Verify class tables exist as globals
assert(type(Animal) == "table")
assert(type(Dog) == "table")
assert(type(GuideDog) == "table")
assert(type(Counter) == "table")
assert(type(Point) == "table")
assert(type(Vec2) == "table")
print("PASS: classes are global tables")

print("")
print("=== All class tests passed! ===")
