/*
** lprofile.c - Built-in profiler for Lua5g
** Tracks function call counts, execution time, and hot spots.
**
** Usage:
**   local profile = require("profile")
**   profile.start()
**   ... run code ...
**   profile.stop()
**   profile.report()      -- print sorted by total time
**   profile.report(10)    -- top 10
**   profile.reset()
*/

#define lprofile_c
#define LUA_CORE

#include "lprefix.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#define PROF_MAX_FUNCS 1024

typedef struct ProfEntry {
  const char *source;    /* source file */
  int linedefined;       /* line where function is defined */
  const char *name;      /* function name (if known) */
  int calls;             /* number of calls */
  double total_time;     /* total time in seconds */
  double self_time;      /* self time (excluding children) */
  struct timespec enter_ts;   /* timestamp when last entered */
  int depth;               /* recursion depth */
} ProfEntry;

static ProfEntry prof_entries[PROF_MAX_FUNCS];
static int prof_count = 0;
static int prof_active = 0;
static struct timespec prof_start_ts;

static ProfEntry *find_or_create (const char *src, int line) {
  int i;
  for (i = 0; i < prof_count; i++) {
    if (prof_entries[i].linedefined == line &&
        prof_entries[i].source != NULL && src != NULL &&
        strcmp(prof_entries[i].source, src) == 0)
      return &prof_entries[i];
  }
  if (prof_count >= PROF_MAX_FUNCS) return NULL;
  ProfEntry *e = &prof_entries[prof_count++];
  e->source = src ? strdup(src) : NULL;
  e->linedefined = line;
  e->name = NULL;
  e->calls = 0;
  e->total_time = 0;
  e->self_time = 0;
  e->enter_ts.tv_sec = 0; e->enter_ts.tv_nsec = 0;
  e->depth = 0;
  return e;
}

static void prof_hook (lua_State *L, lua_Debug *ar) {
  if (!prof_active) return;
  if (ar->event == LUA_HOOKCALL || ar->event == LUA_HOOKTAILCALL) {
    lua_getinfo(L, "Sn", ar);
    ProfEntry *e = find_or_create(ar->short_src, ar->linedefined);
    if (e) {
      e->calls++;
      e->depth++;
      if (e->depth == 1)  /* only time the outermost call */
        clock_gettime(CLOCK_MONOTONIC, &e->enter_ts);
      if (e->name == NULL && ar->name != NULL)
        e->name = strdup(ar->name);
    }
  }
  else if (ar->event == LUA_HOOKRET) {
    lua_getinfo(L, "S", ar);
    ProfEntry *e = find_or_create(ar->short_src, ar->linedefined);
    if (e && e->depth > 0) {
      e->depth--;
      if (e->depth == 0 && e->enter_ts.tv_sec != 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - e->enter_ts.tv_sec) +
                         (now.tv_nsec - e->enter_ts.tv_nsec) / 1e9;
        e->total_time += elapsed;
        e->self_time += elapsed;
        e->enter_ts.tv_sec = 0;
      }
    }
  }
}

static int prof_start (lua_State *L) {
  prof_active = 1;
  clock_gettime(CLOCK_MONOTONIC, &prof_start_ts);
  lua_sethook(L, prof_hook, LUA_MASKCALL | LUA_MASKRET, 0);
  return 0;
}

static int prof_stop (lua_State *L) {
  prof_active = 0;
  lua_sethook(L, NULL, 0, 0);
  return 0;
}

static int prof_reset (lua_State *L) {
  (void)L;
  prof_count = 0;
  memset(prof_entries, 0, sizeof(prof_entries));
  return 0;
}

/* comparison for qsort: by total_time descending */
static int prof_cmp (const void *a, const void *b) {
  double da = ((const ProfEntry *)a)->total_time;
  double db = ((const ProfEntry *)b)->total_time;
  return (da < db) ? 1 : (da > db) ? -1 : 0;
}

static int prof_report (lua_State *L) {
  int top_n = (int)luaL_optinteger(L, 1, 20);
  struct timespec wall_end; clock_gettime(CLOCK_MONOTONIC, &wall_end); double wall = (wall_end.tv_sec - prof_start_ts.tv_sec) + (wall_end.tv_nsec - prof_start_ts.tv_nsec) / 1e9;

  /* sort by total time */
  qsort(prof_entries, (size_t)prof_count, sizeof(ProfEntry), prof_cmp);

  printf("=== Profile Report ===\n");
  printf("Total time: %.4f sec, %d functions tracked\n\n", wall, prof_count);
  printf("%-30s %8s %10s %8s %s\n", "Function", "Calls", "Total(s)", "%", "Source");
  printf("%s\n", "--------------------------------------------------------------------------");

  int n = (top_n < prof_count) ? top_n : prof_count;
  int i;
  for (i = 0; i < n; i++) {
    ProfEntry *e = &prof_entries[i];
    if (e->calls == 0) continue;
    const char *name = e->name ? e->name : "(anonymous)";
    double pct = wall > 0 ? (e->total_time / wall * 100.0) : 0;
    printf("%-30s %8d %10.6f %7.1f%% %s:%d\n",
           name, e->calls, e->total_time, pct,
           e->source ? e->source : "?", e->linedefined);
  }
  printf("\n");
  return 0;
}

/* Get profile data as Lua table */
static int prof_data (lua_State *L) {
  lua_createtable(L, prof_count, 0);
  int i;
  for (i = 0; i < prof_count; i++) {
    ProfEntry *e = &prof_entries[i];
    if (e->calls == 0) continue;
    lua_createtable(L, 0, 5);
    lua_pushstring(L, e->name ? e->name : "(anonymous)");
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, e->calls);
    lua_setfield(L, -2, "calls");
    lua_pushnumber(L, e->total_time);
    lua_setfield(L, -2, "time");
    lua_pushstring(L, e->source ? e->source : "?");
    lua_setfield(L, -2, "source");
    lua_pushinteger(L, e->linedefined);
    lua_setfield(L, -2, "line");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static const luaL_Reg prof_funcs[] = {
  {"start",  prof_start},
  {"stop",   prof_stop},
  {"reset",  prof_reset},
  {"report", prof_report},
  {"data",   prof_data},
  {NULL, NULL}
};

LUAMOD_API int luaopen_profile (lua_State *L) {
  luaL_newlib(L, prof_funcs);
  return 1;
}
