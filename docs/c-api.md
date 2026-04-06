# C Binding API

## Creating a Class

```c
#include "lua.h"
#include "lauxlib.h"

static int player_new(lua_State *L) {
  const char *name = luaL_checkstring(L, 2);
  luaL_pushinstance(L, "Player");
  lua_pushstring(L, name);
  lua_setfield(L, -2, "name");
  return 1;
}

static int player_greet(lua_State *L) {
  luaL_checkinstance(L, 1, "Player");
  lua_getfield(L, 1, "name");
  lua_pushfstring(L, "Hello, %s!", lua_tostring(L, -1));
  return 1;
}

static const luaL_Reg player_methods[] = {
  {"new", player_new},
  {"greet", player_greet},
  {NULL, NULL}
};

// In your init function:
luaL_newclass(L, "Player", player_methods);
```

## API Reference

| Function | Description |
|----------|-------------|
| `luaL_newclass(L, name, methods)` | Create class, register as global |
| `luaL_newsubclass(L, name, parent, methods)` | Create child class |
| `luaL_pushinstance(L, classname)` | Create new instance (table + metatable) |
| `luaL_isinstance(L, idx, classname)` | Check if value is instance (walks metatable chain) |
| `luaL_checkinstance(L, arg, classname)` | Argument check (raises error on mismatch) |
| `luaL_setupclass(L)` | Install access control metamethods |
| `luaL_setclassrelease(L, mode)` | Toggle release mode (skip private/protected checks) |

## Access Control from C

```c
// Create class with access control
luaL_newclass(L, "Secure", methods);

// Set up access metadata
lua_newtable(L);  // __access
lua_pushliteral(L, "private");
lua_setfield(L, -2, "_secret");
lua_setfield(L, -2, "__access");

// Create empty getter/setter tables
lua_newtable(L); lua_setfield(L, -2, "__getters");
lua_newtable(L); lua_setfield(L, -2, "__setters");

// Install access control
luaL_setupclass(L);
```

## Declaration Files

Create `mylib.d.lua` for your C library:

```lua
declare class Player
  name: string
  function new(self, name: string): Player
  function greet(self): string
end
```

Users load it with `lua5g -d mylib.d.lua script.lua` for type checking.

## See Also

- `examples/class_c_binding.c` — Complete Vec2/Vec3 example
