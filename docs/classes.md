# Class System

---

## Basic Class

```lua
class Vec2
  function new(self, x: number, y: number)
    local inst = setmetatable({}, self)
    inst.x = x
    inst.y = y
    return inst
  end

  function length(self): number
    return math.sqrt(self.x^2 + self.y^2)
  end
end

local v = Vec2:new(3, 4)
print(v:length())  -- 5.0
```

---

## Inheritance

```lua
class Dog extends Animal
  override function speak(self): string
    return self.name .. " barks"
  end
end
```

### Override Rules

- Parent has method `foo` → child **must** use `override function foo`
- Without `override` → **compile error**
- `override` on non-existent parent method → **compile error**

### Super

```lua
override function speak(self): string
  return super.speak(self) .. " and barks"
end
```

`super` compiles to the parent class name (zero runtime cost).

---

## RTTI

```lua
local d = Dog:new("Rex")

instanceof(d, Dog)       -- true
instanceof(d, Animal)    -- true (walks inheritance chain)
classname(d)             -- "Dog"
classof(d) == Dog        -- true
parentof(Dog) == Animal  -- true
```

---

## Access Control

```lua
class Account
  public name: string           -- accessible everywhere (default)
  private _balance: number      -- class methods only
  protected _id: number         -- class + subclass methods
  readonly currency: string     -- write once, then immutable
end
```

- **Typed variables**: checked at **compile time** (zero cost)
- **Untyped variables**: checked at **runtime** (debug mode)
- **Release mode**: `__class_release(true)` skips runtime checks

---

## Getter / Setter

```lua
class Player
  private _hp: number

  property hp: number
    get(self) return self._hp end
    set(self, val)
      if val < 0 then val = 0 end
      self._hp = val
    end
  end
end

local p = Player:new()
print(p.hp)    -- calls getter
p.hp = 50      -- calls setter
```

---

## Static & Abstract

```lua
class MathUtil
  static function clamp(val, min, max)
    if val < min then return min end
    if val > max then return max end
    return val
  end
end

MathUtil.clamp(150, 0, 100)  -- no self

class Shape
  abstract function area(self): number
end
```

---

## Operator Overloading

```lua
class Vec2
  operator + (a, b)
    return Vec2:new(a.x + b.x, a.y + b.y)
  end
  operator tostring (self)
    return f"({self.x}, {self.y})"
  end
end
```

Supported operators: `+` `-` `*` `/` `%` `==` `<` `<=` `..` `len` `tostring` `call`

---

## Interface

```lua
interface Serializable
  function serialize(self): string
  function deserialize(self, data: string): nil
end

class Config implements Serializable
  function serialize(self) ... end
  function deserialize(self, data) ... end
  -- missing method → compile error
end
```

---

## Enum

```lua
enum Color
  RED GREEN BLUE
end
-- Color.RED = 1, Color.GREEN = 2, Color.BLUE = 3

enum HttpStatus
  OK = 200
  NOT_FOUND = 404
  ERROR = 500
end
```

---

## Declare Class

For C-bound classes that need type information:

```lua
-- vec2.d.lua
declare class Vec2
  function new(self, x: number, y: number): Vec2
  function length(self): number
end
```

Load with `dala -d vec2.d.lua script.lua`.

---

## Implementation

Classes compile to standard Lua metatable patterns — zero overhead:

```lua
-- class Animal ... end  →
Animal = {}
Animal.__index = Animal
Animal.__name = "Animal"

-- class Dog extends Animal ... end  →
Dog = setmetatable({}, {__index = Animal})
Dog.__index = Dog
Dog.__name = "Dog"
```
