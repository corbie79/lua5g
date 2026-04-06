/*
** ltest.c - Built-in test framework for Lua5g
**
** Usage:
**   local test = require("test")
**   test.describe("MyModule", function()
**     test.it("should add numbers", function()
**       test.expect(1 + 1).toBe(2)
**       test.expect("hello").toContain("ell")
**       test.expect(nil).toBeNil()
**       test.expect(42).toBeType("number")
**     end)
**   end)
**   test.summary()
*/

#define ltest_c
#define LUA_CORE

#include "lprefix.h"

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

static int test_pass = 0;
static int test_fail = 0;
static const char *current_describe = "";
static const char *current_it = "";

/* test.describe(name, func) */
static int test_describe (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  current_describe = name;
  printf("\n  %s\n", name);
  lua_pushvalue(L, 2);
  int status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    printf("    ERROR: %s\n", err ? err : "unknown");
    test_fail++;
    lua_pop(L, 1);
  }
  return 0;
}

/* test.it(name, func) */
static int test_it (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  current_it = name;
  lua_pushvalue(L, 2);
  int status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    printf("    FAIL: %s\n      %s\n", name, err ? err : "unknown");
    test_fail++;
    lua_pop(L, 1);
  }
  else {
    printf("    PASS: %s\n", name);
    test_pass++;
  }
  return 0;
}

/* test.expect(value) - returns a table with assertion methods */
static int expect_toBe (lua_State *L);
static int expect_toEqual (lua_State *L);
static int expect_toBeNil (lua_State *L);
static int expect_toBeType (lua_State *L);
static int expect_toContain (lua_State *L);
static int expect_toBeTruthy (lua_State *L);
static int expect_toBeFalsy (lua_State *L);
static int expect_toThrow (lua_State *L);

static int test_expect (lua_State *L) {
  /* store the value as upvalue 1 for all assertion closures */
  lua_createtable(L, 0, 8);

  lua_pushvalue(L, 1);  /* the value being tested */
  lua_pushcclosure(L, expect_toBe, 1);
  lua_setfield(L, -2, "toBe");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toEqual, 1);
  lua_setfield(L, -2, "toEqual");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toBeNil, 1);
  lua_setfield(L, -2, "toBeNil");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toBeType, 1);
  lua_setfield(L, -2, "toBeType");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toContain, 1);
  lua_setfield(L, -2, "toContain");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toBeTruthy, 1);
  lua_setfield(L, -2, "toBeTruthy");

  lua_pushvalue(L, 1);
  lua_pushcclosure(L, expect_toBeFalsy, 1);
  lua_setfield(L, -2, "toBeFalsy");

  return 1;
}

/* expect(v).toBe(expected) - strict equality */
static int expect_toBe (lua_State *L) {
  /* upvalue 1 = actual, arg 1 = expected */
  int eq;
  if (lua_isinteger(L, lua_upvalueindex(1)) && lua_isinteger(L, 1))
    eq = (lua_tointeger(L, lua_upvalueindex(1)) == lua_tointeger(L, 1));
  else if (lua_isnumber(L, lua_upvalueindex(1)) && lua_isnumber(L, 1))
    eq = (lua_tonumber(L, lua_upvalueindex(1)) == lua_tonumber(L, 1));
  else if (lua_isstring(L, lua_upvalueindex(1)) && lua_isstring(L, 1))
    eq = (strcmp(lua_tostring(L, lua_upvalueindex(1)), lua_tostring(L, 1)) == 0);
  else if (lua_isboolean(L, lua_upvalueindex(1)) && lua_isboolean(L, 1))
    eq = (lua_toboolean(L, lua_upvalueindex(1)) == lua_toboolean(L, 1));
  else if (lua_isnil(L, lua_upvalueindex(1)) && lua_isnil(L, 1))
    eq = 1;
  else
    eq = lua_rawequal(L, lua_upvalueindex(1), 1);
  if (!eq) {
    luaL_tolstring(L, lua_upvalueindex(1), NULL);
    luaL_tolstring(L, 1, NULL);
    return luaL_error(L, "expected %s, got %s",
                     lua_tostring(L, -1), lua_tostring(L, -2));
  }
  return 0;
}

static int expect_toEqual (lua_State *L) { return expect_toBe(L); }

static int expect_toBeNil (lua_State *L) {
  if (!lua_isnil(L, lua_upvalueindex(1))) {
    luaL_tolstring(L, lua_upvalueindex(1), NULL);
    return luaL_error(L, "expected nil, got %s", lua_tostring(L, -1));
  }
  return 0;
}

static int expect_toBeType (lua_State *L) {
  const char *expected = luaL_checkstring(L, 1);
  const char *actual = luaL_typename(L, lua_upvalueindex(1));
  if (strcmp(actual, expected) != 0)
    return luaL_error(L, "expected type '%s', got '%s'", expected, actual);
  return 0;
}

static int expect_toContain (lua_State *L) {
  const char *haystack = lua_tostring(L, lua_upvalueindex(1));
  const char *needle = luaL_checkstring(L, 1);
  if (haystack == NULL || strstr(haystack, needle) == NULL)
    return luaL_error(L, "expected string to contain '%s'", needle);
  return 0;
}

static int expect_toBeTruthy (lua_State *L) {
  if (lua_isnil(L, lua_upvalueindex(1)) ||
      (lua_isboolean(L, lua_upvalueindex(1)) && !lua_toboolean(L, lua_upvalueindex(1))))
    return luaL_error(L, "expected truthy value");
  return 0;
}

static int expect_toBeFalsy (lua_State *L) {
  if (!lua_isnil(L, lua_upvalueindex(1)) &&
      !(lua_isboolean(L, lua_upvalueindex(1)) && !lua_toboolean(L, lua_upvalueindex(1))))
    return luaL_error(L, "expected falsy value");
  return 0;
}

/* test.summary() */
static int test_summary (lua_State *L) {
  (void)L;
  printf("\n  Results: %d passed, %d failed, %d total\n",
         test_pass, test_fail, test_pass + test_fail);
  if (test_fail == 0)
    printf("  ALL TESTS PASSED!\n\n");
  else
    printf("  SOME TESTS FAILED!\n\n");
  int f = test_fail;
  test_pass = test_fail = 0;
  if (f > 0) { lua_pushboolean(L, 0); return 1; }
  lua_pushboolean(L, 1); return 1;
}

static const luaL_Reg test_funcs[] = {
  {"describe", test_describe},
  {"it",       test_it},
  {"expect",   test_expect},
  {"summary",  test_summary},
  {NULL, NULL}
};

LUAMOD_API int luaopen_test (lua_State *L) {
  luaL_newlib(L, test_funcs);
  return 1;
}
