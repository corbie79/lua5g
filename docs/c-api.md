# C Binding API

---

## Creating a Class

```c
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

// Register:
luaL_newclass(L, "Player", player_methods);
```

---

## API Functions

- **`luaL_newclass(L, name, methods)`** — Create class, register as global
- **`luaL_newsubclass(L, name, parent, methods)`** — Create child class
- **`luaL_pushinstance(L, classname)`** — Create instance (table + metatable)
- **`luaL_isinstance(L, idx, classname)`** — Check instanceof (walks chain)
- **`luaL_checkinstance(L, arg, classname)`** — Argument check (raises error)
- **`luaL_setupclass(L)`** — Install access control metamethods
- **`luaL_setclassrelease(L, mode)`** — Toggle release mode

---

## Access Control from C

```c
luaL_newclass(L, "Secure", methods);

// __access table
lua_newtable(L);
lua_pushliteral(L, "private");
lua_setfield(L, -2, "_secret");
lua_setfield(L, -2, "__access");

// Empty getter/setter tables
lua_newtable(L); lua_setfield(L, -2, "__getters");
lua_newtable(L); lua_setfield(L, -2, "__setters");

// Activate
luaL_setupclass(L);
```

---

## Declaration Files

Create `mylib.d.lua`:

```lua
declare class Player
  name: string
  function new(self, name: string): Player
  function greet(self): string
end
```

Usage: `dala -d mylib.d.lua script.lua`

---

## Example

See `examples/class_c_binding.c` for a complete Vec2/Vec3 example with inheritance.
