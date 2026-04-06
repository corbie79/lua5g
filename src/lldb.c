/*
** lldb.c - Debug symbol file (.ldb) generator and loader
** Generates companion debug files with type info, line maps, etc.
** Can be loaded at runtime when errors occur for rich diagnostics.
*/

#define lldb_c
#define LUA_CORE

#include "lprefix.h"

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"

/* .ldb file format:
   Header: "Lua5gDbg\x1a" (9 bytes)
   Version: 1 byte
   Source filename: string
   For each function:
     Line defined: int
     Last line: int
     Num locals: int
     For each local:
       Name: string
       Type: string (or "" if none)
       Start PC: int
       End PC: int
     Num lines: int
     For each line:
       PC: int
       Line: int
*/

#define LDB_HEADER  "Lua5gDbg\x1a"
#define LDB_VERSION 1


/* Write .ldb file for a Proto */
static void write_string (FILE *f, const char *s) {
  if (s == NULL) s = "";
  size_t len = strlen(s);
  fwrite(&len, sizeof(size_t), 1, f);
  fwrite(s, 1, len, f);
}

static void write_int (FILE *f, int v) {
  fwrite(&v, sizeof(int), 1, f);
}

static void write_proto_debug (FILE *f, const Proto *p) {
  int i;
  write_int(f, p->linedefined);
  write_int(f, p->lastlinedefined);
  /* source */
  write_string(f, p->source ? getstr(p->source) : "?");
  /* locals */
  write_int(f, p->sizelocvars);
  for (i = 0; i < p->sizelocvars; i++) {
    write_string(f, p->locvars[i].varname ?
                    getstr(p->locvars[i].varname) : "?");
    write_string(f, p->locvars[i].typename_ ?
                    getstr(p->locvars[i].typename_) : "");
    write_int(f, p->locvars[i].startpc);
    write_int(f, p->locvars[i].endpc);
  }
  /* upvalue names */
  write_int(f, p->sizeupvalues);
  for (i = 0; i < p->sizeupvalues; i++) {
    write_string(f, p->upvalues[i].name ?
                    getstr(p->upvalues[i].name) : "?");
  }
  /* sub-protos */
  write_int(f, p->sizep);
  for (i = 0; i < p->sizep; i++)
    write_proto_debug(f, p->p[i]);
}


/* C API: generate .ldb file from a compiled function */
static int ldb_generate (lua_State *L) {
  const char *outpath = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  /* get Proto from closure */
  lua_Debug ar;
  lua_pushvalue(L, 2);
  lua_getinfo(L, ">S", &ar);
  lua_pushvalue(L, 2);
  const LClosure *cl = lua_topointer(L, -1);
  lua_pop(L, 1);
  if (cl == NULL || cl->p == NULL) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "not a Lua closure");
    return 2;
  }
  FILE *f = fopen(outpath, "wb");
  if (f == NULL) {
    lua_pushboolean(L, 0);
    lua_pushfstring(L, "cannot open '%s'", outpath);
    return 2;
  }
  /* header */
  fwrite(LDB_HEADER, 1, strlen(LDB_HEADER), f);
  fputc(LDB_VERSION, f);
  /* write proto tree */
  write_proto_debug(f, cl->p);
  fclose(f);
  lua_pushboolean(L, 1);
  return 1;
}


/* C API: load .ldb and print debug info */
static int ldb_info (lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot open '%s'", path);
    return 2;
  }
  /* verify header */
  char header[10];
  size_t hlen = strlen(LDB_HEADER);
  if (fread(header, 1, hlen, f) != hlen ||
      memcmp(header, LDB_HEADER, hlen) != 0) {
    fclose(f);
    lua_pushnil(L);
    lua_pushliteral(L, "not a valid .ldb file");
    return 2;
  }
  int ver = fgetc(f);
  /* create result table */
  lua_newtable(L);
  lua_pushinteger(L, ver);
  lua_setfield(L, -2, "version");
  /* read source */
  size_t slen;
  if (fread(&slen, sizeof(size_t), 1, f) == 1 && slen < 4096) {
    char buf[4096];
    if (fread(buf, 1, 2 * sizeof(int), f) == 2 * sizeof(int)) {} /* skip lines */
    if (fread(&slen, sizeof(size_t), 1, f) == 1 && slen < 4096) {
      if (fread(buf, 1, slen, f) == slen) {
        buf[slen] = '\0';
        lua_pushstring(L, buf);
        lua_setfield(L, -2, "source");
      }
    }
  }
  fclose(f);
  return 1;
}


static const luaL_Reg ldb_funcs[] = {
  {"generate", ldb_generate},
  {"info", ldb_info},
  {NULL, NULL}
};

LUAMOD_API int luaopen_ldb (lua_State *L) {
  luaL_newlib(L, ldb_funcs);
  return 1;
}
