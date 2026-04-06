/*
** Example: Binding C classes to Lua using the class API
**
** This shows how to create classes in C that are fully compatible
** with the Lua 'class' keyword and inheritance system.
**
** Compile:
**   gcc -shared -o vec2.so class_c_binding.c -I../src -L../src -llua -lm
**
** Or as a standalone test:
**   gcc -o class_test class_c_binding.c ../src/liblua.a -I../src -lm -ldl
*/

#include <math.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"


/* ============================================================ */
/* Vec2 class - 2D vector                                        */
/* ============================================================ */

/* Vec2:new(x, y) - constructor */
static int vec2_new (lua_State *L) {
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  luaL_pushinstance(L, "Vec2");    /* create new instance */
  lua_pushnumber(L, x);
  lua_setfield(L, -2, "x");       /* inst.x = x */
  lua_pushnumber(L, y);
  lua_setfield(L, -2, "y");       /* inst.y = y */
  return 1;
}

/* Vec2:length() */
static int vec2_length (lua_State *L) {
  luaL_checkinstance(L, 1, "Vec2");
  lua_getfield(L, 1, "x");
  lua_getfield(L, 1, "y");
  lua_Number x = lua_tonumber(L, -2);
  lua_Number y = lua_tonumber(L, -1);
  lua_pushnumber(L, sqrt(x * x + y * y));
  return 1;
}

/* Vec2:add(other) - returns new Vec2 */
static int vec2_add (lua_State *L) {
  luaL_checkinstance(L, 1, "Vec2");
  luaL_checkinstance(L, 2, "Vec2");
  lua_getfield(L, 1, "x"); lua_getfield(L, 2, "x");
  lua_getfield(L, 1, "y"); lua_getfield(L, 2, "y");
  lua_Number x = lua_tonumber(L, -4) + lua_tonumber(L, -3);
  lua_Number y = lua_tonumber(L, -2) + lua_tonumber(L, -1);
  lua_pop(L, 4);
  /* create result via Vec2:new */
  luaL_pushinstance(L, "Vec2");
  lua_pushnumber(L, x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, y); lua_setfield(L, -2, "y");
  return 1;
}

/* Vec2:tostring() */
static int vec2_tostring (lua_State *L) {
  lua_getfield(L, 1, "x");
  lua_getfield(L, 1, "y");
  char buf[128];
  snprintf(buf, sizeof(buf), "Vec2(%.2g, %.2g)",
           lua_tonumber(L, -2), lua_tonumber(L, -1));
  lua_pushstring(L, buf);
  return 1;
}

static const luaL_Reg vec2_methods[] = {
  {"new",       vec2_new},
  {"length",    vec2_length},
  {"add",       vec2_add},
  {"__tostring", vec2_tostring},
  {NULL, NULL}
};


/* ============================================================ */
/* Vec3 class - extends Vec2                                     */
/* ============================================================ */

/* Vec3:new(x, y, z) */
static int vec3_new (lua_State *L) {
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number z = luaL_checknumber(L, 4);
  luaL_pushinstance(L, "Vec3");
  lua_pushnumber(L, x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, y); lua_setfield(L, -2, "y");
  lua_pushnumber(L, z); lua_setfield(L, -2, "z");
  return 1;
}

/* Vec3:length() - override to include z */
static int vec3_length (lua_State *L) {
  luaL_checkinstance(L, 1, "Vec3");
  lua_getfield(L, 1, "x");
  lua_getfield(L, 1, "y");
  lua_getfield(L, 1, "z");
  lua_Number x = lua_tonumber(L, -3);
  lua_Number y = lua_tonumber(L, -2);
  lua_Number z = lua_tonumber(L, -1);
  lua_pushnumber(L, sqrt(x * x + y * y + z * z));
  return 1;
}

/* Vec3:tostring() */
static int vec3_tostring (lua_State *L) {
  lua_getfield(L, 1, "x");
  lua_getfield(L, 1, "y");
  lua_getfield(L, 1, "z");
  char buf[128];
  snprintf(buf, sizeof(buf), "Vec3(%.2g, %.2g, %.2g)",
           lua_tonumber(L, -3), lua_tonumber(L, -2), lua_tonumber(L, -1));
  lua_pushstring(L, buf);
  return 1;
}

static const luaL_Reg vec3_methods[] = {
  {"new",        vec3_new},
  {"length",     vec3_length},
  {"__tostring", vec3_tostring},
  /* Note: 'add' is inherited from Vec2 */
  {NULL, NULL}
};


/* ============================================================ */
/* Standalone test                                                */
/* ============================================================ */

#ifdef CLASS_TEST_STANDALONE

int main (void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  /* register classes from C */
  luaL_newclass(L, "Vec2", vec2_methods);
  lua_pop(L, 1);

  luaL_newsubclass(L, "Vec3", "Vec2", vec3_methods);
  lua_pop(L, 1);

  /* run Lua test code */
  const char *code =
    "-- Test C-bound classes\n"
    "local v1 = Vec2:new(3, 4)\n"
    "print(tostring(v1))\n"
    "print('length:', v1:length())\n"
    "\n"
    "local v2 = Vec2:new(1, 2)\n"
    "local v3 = v1:add(v2)\n"
    "print('add:', tostring(v3))\n"
    "\n"
    "local v4 = Vec3:new(1, 2, 3)\n"
    "print(tostring(v4))\n"
    "print('length:', v4:length())\n"
    "\n"
    "-- Type checking works with C classes too\n"
    "local a: Vec2 = Vec2:new(5, 6)\n"
    "print('typed:', tostring(a))\n"
    "\n"
    "print('\\nAll C class tests passed!')\n";

  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return 1;
  }

  lua_close(L);
  return 0;
}

#endif
