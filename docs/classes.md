# Class System

## Basic Class

```lua
class Animal
  name: string
  age: number

  function new(self, name: string, age: number)
    local inst = setmetatable({}, self)
    inst.name = name
    inst.age = age
    return inst
  end

  function speak(self): string
    return self.name .. " speaks"
  end
end

local cat = Animal:new("Whiskers", 5)
print(cat:speak())  -- "Whiskers speaks"
```

## Inheritance

```lua
class Dog extends Animal
  breed: string

  override function speak(self): string
    return self.name .. " barks"
  end
end
```

## Override Rules

- If parent has method `foo`, child MUST use `override function foo` to redefine
- Without `override`: compile error
- `override` on non-existent parent method: compile error

## Super

```lua
class Dog extends Animal
  override function speak(self): string
    return super.speak(self) .. " and barks"
  end
end
```

`super` resolves to the parent class inside method bodies.

## Access Modifiers

| Modifier | Meaning |
|----------|---------|
| `public` | Accessible everywhere (default) |
| `private` | Class methods only |
| `protected` | Class + subclass methods |
| `readonly` | Write once (during construction), then immutable |

```lua
class Account
  public name: string
  private _balance: number
  protected _id: number
  readonly currency: string
end
```

Access is checked at **compile time** for typed variables, and at **runtime** for untyped:

```lua
local acc: Account = Account:new(...)
print(acc._balance)  -- COMPILE ERROR: cannot access private field
```

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
print(p.hp)      -- calls getter
p.hp = 50        -- calls setter
```

## Static Methods

```lua
class MathUtil
  static function clamp(val, min, max)
    if val < min then return min end
    if val > max then return max end
    return val
  end
end

MathUtil.clamp(150, 0, 100)  -- 100 (no self)
```

## Abstract Methods

```lua
class Shape
  abstract function area(self): number
  abstract function perimeter(self): number

  function describe(self)
    return f"Area: {self:area()}"
  end
end
```

Abstract methods have no body. Subclasses are expected to implement them (checked via `implements`).

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

| Operator | Metamethod |
|----------|-----------|
| `+` | `__add` |
| `-` | `__sub` |
| `*` | `__mul` |
| `/` | `__div` |
| `%` | `__mod` |
| `==` | `__eq` |
| `<` | `__lt` |
| `<=` | `__le` |
| `..` | `__concat` |
| `len` | `__len` |
| `tostring` | `__tostring` |
| `call` | `__call` |

## Interface

```lua
interface Serializable
  function serialize(self): string
  function deserialize(self, data: string): nil
end

class Config implements Serializable
  function serialize(self) return json.encode(self.data) end
  function deserialize(self, data) self.data = json.decode(data) end
end
```

Missing methods → compile error.

## Enum

```lua
enum Color
  RED
  GREEN
  BLUE
end
-- Color.RED = 1, Color.GREEN = 2, Color.BLUE = 3

enum HttpStatus
  OK = 200
  NOT_FOUND = 404
  ERROR = 500
end
```

## Declare Class

For C-bound classes that exist at runtime but need type information:

```lua
-- vec2.d.lua
declare class Vec2
  function new(self, x: number, y: number): Vec2
  function length(self): number
  function add(self, other: Vec2): Vec2
end
```

Load with: `lua5g -d vec2.d.lua script.lua`

## Implementation

Classes compile to standard Lua table + metatable patterns:

```lua
-- class Animal ... end  →
Animal = {}
Animal.__index = Animal

-- class Dog extends Animal ... end  →
Dog = setmetatable({}, {__index = Animal})
Dog.__index = Dog
```

Zero overhead compared to hand-written metatable code.
