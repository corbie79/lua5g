/*
** lmathx.c - Extended math operations with batch processing
** Provides high-performance C implementations for common patterns
** that avoid per-element Lua->C call overhead.
**
** Usage:
**   local mathx = require("mathx")
**   mathx.map_sqrt(array, n)        -- in-place sqrt on array[1..n]
**   mathx.dot(a, b, n)              -- dot product
**   mathx.sum(array, n)             -- sum array elements
**   mathx.transform(array, n, fn)   -- apply fn to each element
*/

#include <math.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* ============================================================ */
/* Batch math operations (avoid per-call overhead)               */
/* ============================================================ */

/* Sum of array elements: sum(t, n) */
static int mathx_sum (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Number sum = 0.0;
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 1, i);
    sum += lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  lua_pushnumber(L, sum);
  return 1;
}

/* Dot product: dot(a, b, n) */
static int mathx_dot (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_Integer n = luaL_checkinteger(L, 3);
  lua_Number sum = 0.0;
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 1, i);
    lua_rawgeti(L, 2, i);
    sum += lua_tonumber(L, -2) * lua_tonumber(L, -1);
    lua_pop(L, 2);
  }
  lua_pushnumber(L, sum);
  return 1;
}

/* In-place sqrt: map_sqrt(t, n) */
static int mathx_map_sqrt (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 1, i);
    lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_pushnumber(L, sqrt(v));
    lua_rawseti(L, 1, i);
  }
  return 0;
}

/* In-place transform: map(t, n, fn) -- fn is index to operation:
   1=sqrt, 2=abs, 3=sin, 4=cos, 5=exp, 6=log */
static int mathx_map (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Integer op = luaL_checkinteger(L, 3);
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 1, i);
    lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_Number r;
    switch (op) {
      case 1: r = sqrt(v); break;
      case 2: r = fabs(v); break;
      case 3: r = sin(v); break;
      case 4: r = cos(v); break;
      case 5: r = exp(v); break;
      case 6: r = log(v); break;
      default: r = v; break;
    }
    lua_pushnumber(L, r);
    lua_rawseti(L, 1, i);
  }
  return 0;
}

/* Normalize vector array in-place: normalize(t, n, stride)
   Treats t as array of stride-dimensional vectors */
static int mathx_normalize (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Integer stride = luaL_optinteger(L, 3, 3);
  lua_Integer i, j;
  for (i = 0; i < n; i++) {
    lua_Number len2 = 0;
    for (j = 0; j < stride; j++) {
      lua_rawgeti(L, 1, i * stride + j + 1);
      lua_Number v = lua_tonumber(L, -1);
      len2 += v * v;
      lua_pop(L, 1);
    }
    lua_Number inv = 1.0 / sqrt(len2);
    for (j = 0; j < stride; j++) {
      lua_rawgeti(L, 1, i * stride + j + 1);
      lua_Number v = lua_tonumber(L, -1);
      lua_pop(L, 1);
      lua_pushnumber(L, v * inv);
      lua_rawseti(L, 1, i * stride + j + 1);
    }
  }
  return 0;
}

/* Saxpy: y = a*x + y (arrays) */
static int mathx_saxpy (lua_State *L) {
  lua_Number a = luaL_checknumber(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);  /* x */
  luaL_checktype(L, 3, LUA_TTABLE);  /* y */
  lua_Integer n = luaL_checkinteger(L, 4);
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 2, i);
    lua_rawgeti(L, 3, i);
    lua_Number xi = lua_tonumber(L, -2);
    lua_Number yi = lua_tonumber(L, -1);
    lua_pop(L, 2);
    lua_pushnumber(L, a * xi + yi);
    lua_rawseti(L, 3, i);
  }
  return 0;
}

/* Matrix multiply: C = A * B (flat arrays, size m x k, k x n) */
static int mathx_matmul (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);  /* A */
  luaL_checktype(L, 2, LUA_TTABLE);  /* B */
  lua_Integer m = luaL_checkinteger(L, 3);
  lua_Integer k = luaL_checkinteger(L, 4);
  lua_Integer n = luaL_checkinteger(L, 5);
  lua_Integer i, j, p;
  lua_createtable(L, (int)(m * n), 0);  /* result C */
  for (i = 0; i < m; i++) {
    for (j = 0; j < n; j++) {
      lua_Number s = 0;
      for (p = 0; p < k; p++) {
        lua_rawgeti(L, 1, (int)(i * k + p + 1));
        lua_rawgeti(L, 2, (int)(p * n + j + 1));
        s += lua_tonumber(L, -2) * lua_tonumber(L, -1);
        lua_pop(L, 2);
      }
      lua_pushnumber(L, s);
      lua_rawseti(L, -2, (int)(i * n + j + 1));
    }
  }
  return 1;
}


/* ============================================================ */
/* Native float array: zero-copy C array for max performance     */
/* ============================================================ */

#define FARRAY_MT "mathx.farray"

typedef struct {
  lua_Integer len;
  lua_Number data[];  /* flexible array member */
} FloatArray;

static FloatArray *check_farray (lua_State *L, int idx) {
  return (FloatArray *)luaL_checkudata(L, idx, FARRAY_MT);
}

/* mathx.farray(n [, init]) - create native float array */
static int mathx_farray_new (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  lua_Number init = luaL_optnumber(L, 2, 0.0);
  FloatArray *a = (FloatArray *)lua_newuserdatauv(L,
      sizeof(FloatArray) + (size_t)n * sizeof(lua_Number), 0);
  a->len = n;
  for (lua_Integer i = 0; i < n; i++)
    a->data[i] = init;
  luaL_setmetatable(L, FARRAY_MT);
  return 1;
}

/* farray:set(i, v) */
static int farray_set (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2) - 1;
  lua_Number v = luaL_checknumber(L, 3);
  if (i >= 0 && i < a->len)
    a->data[i] = v;
  return 0;
}

/* farray:get(i) */
static int farray_get (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2) - 1;
  if (i >= 0 && i < a->len)
    lua_pushnumber(L, a->data[i]);
  else
    lua_pushnil(L);
  return 1;
}

/* farray:len() */
static int farray_len (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  lua_pushinteger(L, a->len);
  return 1;
}

/* farray:sum() - pure C, no Lua table overhead */
static int farray_sum (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  lua_Number s = 0;
  for (lua_Integer i = 0; i < a->len; i++)
    s += a->data[i];
  lua_pushnumber(L, s);
  return 1;
}

/* farray:dot(other) */
static int farray_dot (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  FloatArray *b = check_farray(L, 2);
  lua_Integer n = (a->len < b->len) ? a->len : b->len;
  lua_Number s = 0;
  for (lua_Integer i = 0; i < n; i++)
    s += a->data[i] * b->data[i];
  lua_pushnumber(L, s);
  return 1;
}

/* farray:map_sqrt() - in-place, pure C loop */
static int farray_map_sqrt (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  for (lua_Integer i = 0; i < a->len; i++)
    a->data[i] = sqrt(a->data[i]);
  return 0;
}

/* farray:saxpy(alpha, x) -- self = alpha * x + self */
static int farray_saxpy (lua_State *L) {
  FloatArray *y = check_farray(L, 1);
  lua_Number alpha = luaL_checknumber(L, 2);
  FloatArray *x = check_farray(L, 3);
  lua_Integer n = (y->len < x->len) ? y->len : x->len;
  for (lua_Integer i = 0; i < n; i++)
    y->data[i] = alpha * x->data[i] + y->data[i];
  return 0;
}

/* farray:fill_range(start, step) */
static int farray_fill_range (lua_State *L) {
  FloatArray *a = check_farray(L, 1);
  lua_Number start = luaL_optnumber(L, 2, 1.0);
  lua_Number step = luaL_optnumber(L, 3, 1.0);
  for (lua_Integer i = 0; i < a->len; i++)
    a->data[i] = start + (lua_Number)i * step;
  return 0;
}

static const luaL_Reg farray_methods[] = {
  {"set", farray_set},
  {"get", farray_get},
  {"len", farray_len},
  {"sum", farray_sum},
  {"dot", farray_dot},
  {"map_sqrt", farray_map_sqrt},
  {"saxpy", farray_saxpy},
  {"fill_range", farray_fill_range},
  {NULL, NULL}
};


static const luaL_Reg mathx_funcs[] = {
  {"sum",       mathx_sum},
  {"dot",       mathx_dot},
  {"map_sqrt",  mathx_map_sqrt},
  {"map",       mathx_map},
  {"normalize", mathx_normalize},
  {"saxpy",     mathx_saxpy},
  {"matmul",    mathx_matmul},
  {NULL, NULL}
};

LUAMOD_API int luaopen_mathx (lua_State *L) {
  /* create farray metatable */
  luaL_newmetatable(L, FARRAY_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");  /* mt.__index = mt */
  luaL_setfuncs(L, farray_methods, 0);
  lua_pop(L, 1);

  luaL_newlib(L, mathx_funcs);
  /* add farray constructor */
  lua_pushcfunction(L, mathx_farray_new);
  lua_setfield(L, -2, "farray");
  /* add operation constants */
  lua_pushinteger(L, 1); lua_setfield(L, -2, "SQRT");
  lua_pushinteger(L, 2); lua_setfield(L, -2, "ABS");
  lua_pushinteger(L, 3); lua_setfield(L, -2, "SIN");
  lua_pushinteger(L, 4); lua_setfield(L, -2, "COS");
  lua_pushinteger(L, 5); lua_setfield(L, -2, "EXP");
  lua_pushinteger(L, 6); lua_setfield(L, -2, "LOG");
  return 1;
}
