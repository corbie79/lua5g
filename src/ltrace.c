/*
** ltrace.c - Enhanced error tracing with local variable values
** For live service debugging: dumps all locals at each stack frame
** when an error occurs.
**
** Usage:
**   trace.enable()              -- install enhanced error handler
**   trace.disable()             -- restore default handler
**   trace.handler(err)          -- manual call for pcall
**   trace.dump()                -- dump current stack (no error)
**   trace.setid(func)           -- custom trace ID generator
*/

#define ltrace_c
#define LUA_CORE

#include "lprefix.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"


/* max depth of stack to inspect */
#define TRACE_MAXDEPTH  20
/* max locals per frame */
#define TRACE_MAXLOCALS 30
/* max string repr length for a value */
#define TRACE_MAXVALLEN 128


/*
** Generate a short trace ID from timestamp + random-ish bits
*/
static void gen_traceid (char *buf, size_t bufsz) {
  unsigned long t = (unsigned long)time(NULL);
  unsigned long h = t * 2654435761UL;  /* Knuth multiplicative hash */
  snprintf(buf, bufsz, "%08lx", h & 0xFFFFFFFF);
}


/*
** Format a Lua value as a short string for display.
** Handles: nil, bool, number, string (truncated), table (type+addr),
** function (source:line), userdata (addr)
*/
static void format_value (lua_State *L, int idx, char *buf, size_t bufsz) {
  int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNIL:
      snprintf(buf, bufsz, "nil");
      break;
    case LUA_TBOOLEAN:
      snprintf(buf, bufsz, "%s", lua_toboolean(L, idx) ? "true" : "false");
      break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx))
        snprintf(buf, bufsz, "%lld", (long long)lua_tointeger(L, idx));
      else
        snprintf(buf, bufsz, "%.6g", lua_tonumber(L, idx));
      break;
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      if (len > 40)
        snprintf(buf, bufsz, "\"%.37s...\"", s);
      else
        snprintf(buf, bufsz, "\"%s\"", s);
      break;
    }
    case LUA_TTABLE: {
      /* try to get __name from metatable for class instances */
      if (luaL_getmetafield(L, idx, "__name")) {
        const char *name = lua_tostring(L, -1);
        snprintf(buf, bufsz, "%s: %p", name ? name : "table",
                 lua_topointer(L, idx));
        lua_pop(L, 1);
      }
      else {
        /* show first few fields */
        int count = 0;
        size_t pos = 0;
        pos += snprintf(buf + pos, bufsz - pos, "{");
        lua_pushnil(L);
        while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0 && count < 3) {
          if (count > 0) pos += snprintf(buf + pos, bufsz - pos, ", ");
          /* key */
          if (lua_type(L, -2) == LUA_TSTRING)
            pos += snprintf(buf + pos, bufsz - pos, "%s=", lua_tostring(L, -2));
          /* value (short) */
          if (lua_type(L, -1) == LUA_TNUMBER) {
            if (lua_isinteger(L, -1))
              pos += snprintf(buf + pos, bufsz - pos, "%lld",
                             (long long)lua_tointeger(L, -1));
            else
              pos += snprintf(buf + pos, bufsz - pos, "%.4g",
                             lua_tonumber(L, -1));
          }
          else if (lua_type(L, -1) == LUA_TSTRING)
            pos += snprintf(buf + pos, bufsz - pos, "\"%.10s\"",
                           lua_tostring(L, -1));
          else
            pos += snprintf(buf + pos, bufsz - pos, "%s",
                           luaL_typename(L, -1));
          lua_pop(L, 1);
          count++;
          if (pos >= bufsz - 10) break;
        }
        if (count == 0)
          snprintf(buf + pos, bufsz - pos, "}");
        else
          snprintf(buf + pos, bufsz - pos, "%s}", count >= 3 ? ", ..." : "");
      }
      break;
    }
    case LUA_TFUNCTION: {
      lua_Debug ar;
      lua_pushvalue(L, idx);
      lua_getinfo(L, ">S", &ar);
      if (ar.source && ar.linedefined > 0)
        snprintf(buf, bufsz, "function@%s:%d", ar.short_src, ar.linedefined);
      else
        snprintf(buf, bufsz, "function: %p", lua_topointer(L, idx));
      break;
    }
    default:
      snprintf(buf, bufsz, "%s: %p", lua_typename(L, t),
               lua_topointer(L, idx));
      break;
  }
}


/*
** Build enhanced stack trace string with local variable values.
*/
static int trace_handler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) msg = "(non-string error)";

  luaL_Buffer b;
  luaL_buffinit(L, &b);

  /* trace ID */
  char traceid[16];
  gen_traceid(traceid, sizeof(traceid));

  luaL_addstring(&b, "ERROR [");
  luaL_addstring(&b, traceid);
  luaL_addstring(&b, "] ");
  luaL_addstring(&b, msg);
  luaL_addstring(&b, "\n");

  /* walk stack frames */
  lua_Debug ar;
  int level;
  for (level = 1; level < TRACE_MAXDEPTH; level++) {
    if (!lua_getstack(L, level, &ar)) break;
    lua_getinfo(L, "Slnf", &ar);

    /* frame header */
    char frame[256];
    if (ar.currentline > 0)
      snprintf(frame, sizeof(frame), "  %s:%d in %s '%s'\n",
               ar.short_src, ar.currentline,
               ar.namewhat ? ar.namewhat : "?",
               ar.name ? ar.name : "?");
    else
      snprintf(frame, sizeof(frame), "  %s in %s '%s'\n",
               ar.short_src,
               ar.namewhat ? ar.namewhat : "?",
               ar.name ? ar.name : "?");
    luaL_addstring(&b, frame);

    /* dump local variables */
    const char *name;
    int i;
    for (i = 1; i <= TRACE_MAXLOCALS; i++) {
      name = lua_getlocal(L, &ar, i);
      if (name == NULL) break;
      /* skip internal vars (starting with '(') */
      if (name[0] == '(') {
        lua_pop(L, 1);
        continue;
      }
      char valbuf[TRACE_MAXVALLEN];
      format_value(L, -1, valbuf, sizeof(valbuf));
      lua_pop(L, 1);

      char line[256];
      snprintf(line, sizeof(line), "      %s = %s\n", name, valbuf);
      luaL_addstring(&b, line);
    }
  }

  luaL_addstring(&b, "  Trace ID: ");
  luaL_addstring(&b, traceid);
  luaL_pushresult(&b);
  return 1;
}


/*
** Encode a local variable's name+value as an obfuscated token.
** Without the .ldb file, this is meaningless hex.
** With .ldb, it can be decoded to "varname = value".
**
** Format: VAR[slot_hash:value_hash:type_tag]
** The slot_hash is derived from variable index + pc,
** so only .ldb (which has varname↔slot mapping) can decode it.
*/
static void encode_var_token (char *buf, size_t bufsz,
                               int slot, int pc, int valtype,
                               lua_Integer ival, const char *sval) {
  /* hash slot+pc to hide variable identity */
  unsigned long sh = ((unsigned long)slot * 2654435761UL) ^
                     ((unsigned long)pc * 40503UL);
  sh &= 0xFFFF;

  /* hash the value */
  unsigned long vh;
  switch (valtype) {
    case LUA_TNUMBER:
      vh = (unsigned long)ival ^ 0xDEAD;
      break;
    case LUA_TSTRING:
      vh = 0;
      if (sval) {
        for (int i = 0; sval[i] && i < 32; i++)
          vh = vh * 31 + (unsigned char)sval[i];
      }
      break;
    case LUA_TNIL:
      vh = 0;
      break;
    case LUA_TBOOLEAN:
      vh = (unsigned long)ival;
      break;
    default:
      vh = (unsigned long)ival & 0xFFFF;
      break;
  }
  vh &= 0xFFFF;

  snprintf(buf, bufsz, "V[%04lx:%04lx:%x]", sh, vh, valtype);
}


/*
** Build OBFUSCATED stack trace - values are encoded,
** need .ldb to decode. Safe for client-facing error messages.
*/
static int trace_encoded_handler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) msg = "(error)";

  luaL_Buffer b;
  luaL_buffinit(L, &b);

  char traceid[16];
  gen_traceid(traceid, sizeof(traceid));

  luaL_addstring(&b, "E[");
  luaL_addstring(&b, traceid);
  luaL_addstring(&b, "] ");
  /* only show error type, not full message (may contain sensitive data) */
  {
    /* extract just the error type: "attempt to X" → "ERR_INDEX" etc */
    if (strstr(msg, "nil value"))
      luaL_addstring(&b, "NIL_ACCESS");
    else if (strstr(msg, "number"))
      luaL_addstring(&b, "TYPE_ERR");
    else if (strstr(msg, "stack overflow"))
      luaL_addstring(&b, "STACK_OVF");
    else
      luaL_addstring(&b, "RUNTIME");
  }
  luaL_addstring(&b, "\n");

  /* walk stack: emit encoded variable tokens */
  lua_Debug ar;
  int level;
  for (level = 1; level < TRACE_MAXDEPTH; level++) {
    if (!lua_getstack(L, level, &ar)) break;
    lua_getinfo(L, "Sln", &ar);

    char frame[64];
    /* show file hash + line (not full path - hide server structure) */
    unsigned long fhash = 0;
    if (ar.short_src) {
      for (int i = 0; ar.short_src[i]; i++)
        fhash = fhash * 31 + (unsigned char)ar.short_src[i];
    }
    snprintf(frame, sizeof(frame), " F[%04lx:%d]",
             fhash & 0xFFFF, ar.currentline);
    luaL_addstring(&b, frame);

    /* dump encoded locals */
    const char *name;
    int i;
    for (i = 1; i <= TRACE_MAXLOCALS; i++) {
      name = lua_getlocal(L, &ar, i);
      if (name == NULL) break;
      if (name[0] == '(') { lua_pop(L, 1); continue; }

      int vt = lua_type(L, -1);
      lua_Integer iv = 0;
      const char *sv = NULL;
      if (vt == LUA_TNUMBER && lua_isinteger(L, -1))
        iv = lua_tointeger(L, -1);
      else if (vt == LUA_TSTRING)
        sv = lua_tostring(L, -1);
      else if (vt == LUA_TBOOLEAN)
        iv = lua_toboolean(L, -1);
      lua_pop(L, 1);

      char tok[32];
      encode_var_token(tok, sizeof(tok), i, ar.currentline, vt, iv, sv);
      luaL_addchar(&b, ' ');
      luaL_addstring(&b, tok);
    }
    luaL_addstring(&b, "\n");
  }

  luaL_pushresult(&b);
  return 1;
}


/*
** trace.setlogger(func) - set a Lua callback for trace output
** This callback receives the FULL trace (server-side logging).
** The error returned to caller only has Trace ID (client-safe).
*/
static int trace_setlogger (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_setfield(L, LUA_REGISTRYINDEX, "__trace_logger");
  return 0;
}


/*
** Safe error handler: logs full trace to server, returns only Trace ID
*/
static int trace_safe_handler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) msg = "(error)";

  /* generate trace ID */
  char traceid[16];
  gen_traceid(traceid, sizeof(traceid));

  /* build full trace (for server log only) */
  lua_pushcfunction(L, trace_handler);
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);  /* get full trace string */

  /* call logger callback if set */
  lua_getfield(L, LUA_REGISTRYINDEX, "__trace_logger");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -2);  /* full trace */
    lua_pushstring(L, traceid);
    lua_call(L, 2, 0);  /* logger(full_trace, trace_id) */
  }
  else {
    lua_pop(L, 1);
    /* no logger: write to stderr */
    const char *full = lua_tostring(L, -1);
    if (full) fprintf(stderr, "%s\n", full);
  }
  lua_pop(L, 1);  /* pop full trace */

  /* return ONLY trace ID to caller (safe for client) */
  lua_pushfstring(L, "Internal error [%s]", traceid);
  return 1;
}


/*
** trace.enable(mode)
**   "full"   - show everything (dev/server mode)
**   "safe"   - show only trace ID, log full trace via logger (production)
*/
static int trace_enable (lua_State *L) {
  const char *mode = luaL_optstring(L, 1, "full");
  if (strcmp(mode, "safe") == 0) {
    lua_pushcfunction(L, trace_safe_handler);
  }
  else if (strcmp(mode, "encoded") == 0) {
    lua_pushcfunction(L, trace_encoded_handler);
  }
  else {
    lua_pushcfunction(L, trace_handler);
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "__trace_handler");
  return 0;
}

static int trace_disable (lua_State *L) {
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "__trace_handler");
  return 0;
}

/*
** trace.dump() - dump current stack without error
*/
static int trace_dump (lua_State *L) {
  lua_pushliteral(L, "stack dump (no error)");
  return trace_handler(L);
}

/*
** trace.pcall(func, ...) - pcall with enhanced trace on error
*/
static int trace_pcall (lua_State *L) {
  int nargs = lua_gettop(L) - 1;
  /* use registered handler (respects enable mode) */
  lua_getfield(L, LUA_REGISTRYINDEX, "__trace_handler");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    lua_pushcfunction(L, trace_handler);  /* default: full */
  }
  lua_insert(L, 1);
  int status = lua_pcall(L, nargs, LUA_MULTRET, 1);
  lua_remove(L, 1);
  lua_pushboolean(L, status == LUA_OK);
  lua_insert(L, 1);
  return lua_gettop(L);
}


static const luaL_Reg trace_funcs[] = {
  {"enable",    trace_enable},
  {"disable",   trace_disable},
  {"handler",   trace_handler},
  {"dump",      trace_dump},
  {"pcall",     trace_pcall},
  {"setlogger", trace_setlogger},
  {"encoded",   trace_encoded_handler},
  {NULL, NULL}
};

LUAMOD_API int luaopen_trace (lua_State *L) {
  luaL_newlib(L, trace_funcs);
  return 1;
}
