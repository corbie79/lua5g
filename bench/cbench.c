/*
** C math benchmark module - loaded as shared library
** Provides optimized C implementations for comparison with Lua
*/
#include <math.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* ============================================================ */
/* 1. Raw arithmetic in C                                        */
/* ============================================================ */

/* sum 1..N in C */
static int bench_sum (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  lua_Integer sum = 0;
  lua_Integer i;
  for (i = 1; i <= n; i++)
    sum += i;
  lua_pushinteger(L, sum);
  return 1;
}

/* fibonacci(n) iterative in C */
static int bench_fib (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  lua_Integer a = 0, b = 1, t;
  lua_Integer i;
  for (i = 0; i < n; i++) {
    t = a + b;
    a = b;
    b = t;
  }
  lua_pushinteger(L, a);
  return 1;
}

/* N-body math: compute gravitational force components (float heavy) */
static int bench_nbody_step (lua_State *L) {
  lua_Number x1 = luaL_checknumber(L, 1);
  lua_Number y1 = luaL_checknumber(L, 2);
  lua_Number x2 = luaL_checknumber(L, 3);
  lua_Number y2 = luaL_checknumber(L, 4);
  lua_Number mass = luaL_checknumber(L, 5);
  lua_Integer iters = luaL_checkinteger(L, 6);
  lua_Number fx = 0, fy = 0;
  lua_Integer i;
  for (i = 0; i < iters; i++) {
    lua_Number dx = x2 - x1 + (lua_Number)i * 0.001;
    lua_Number dy = y2 - y1 + (lua_Number)i * 0.001;
    lua_Number dist2 = dx * dx + dy * dy + 0.01;
    lua_Number inv = mass / (dist2 * sqrt(dist2));
    fx += dx * inv;
    fy += dy * inv;
  }
  lua_pushnumber(L, fx);
  lua_pushnumber(L, fy);
  return 2;
}

/* matrix 4x4 multiply (passed as flat arrays) */
static int bench_mat4_mul (lua_State *L) {
  lua_Number a[16], b[16], c[16];
  int i, j, k;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  for (i = 0; i < 16; i++) {
    lua_rawgeti(L, 1, i + 1);
    a[i] = lua_tonumber(L, -1);
    lua_rawgeti(L, 2, i + 1);
    b[i] = lua_tonumber(L, -1);
    lua_pop(L, 2);
  }
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++) {
      lua_Number s = 0;
      for (k = 0; k < 4; k++)
        s += a[i * 4 + k] * b[k * 4 + j];
      c[i * 4 + j] = s;
    }
  lua_createtable(L, 16, 0);
  for (i = 0; i < 16; i++) {
    lua_pushnumber(L, c[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* tight loop: increment counter N times (measure call overhead) */
static int bench_noop_loop (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  lua_Integer i;
  volatile lua_Integer x = 0;
  for (i = 0; i < n; i++)
    x++;
  lua_pushinteger(L, x);
  return 1;
}

/* Vec2 operations entirely in C */
static int bench_vec2_ops (lua_State *L) {
  lua_Integer iters = luaL_checkinteger(L, 1);
  lua_Number x = 1.0, y = 2.0;
  lua_Integer i;
  for (i = 0; i < iters; i++) {
    lua_Number nx = x * 0.99 + y * 0.01;
    lua_Number ny = y * 0.99 - x * 0.01;
    lua_Number len = sqrt(nx * nx + ny * ny);
    x = nx / len;
    y = ny / len;
  }
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  return 2;
}


static const luaL_Reg bench_funcs[] = {
  {"sum", bench_sum},
  {"fib", bench_fib},
  {"nbody_step", bench_nbody_step},
  {"mat4_mul", bench_mat4_mul},
  {"noop_loop", bench_noop_loop},
  {"vec2_ops", bench_vec2_ops},
  {NULL, NULL}
};

int luaopen_cbench (lua_State *L) {
  luaL_newlib(L, bench_funcs);
  return 1;
}
