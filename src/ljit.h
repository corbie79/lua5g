/*
** ljit.h - Lightweight JIT compiler for Lua5g
** Compiles hot numeric loops to native machine code.
** Supports: x86-64, ARM64 (with architecture backends)
*/

#ifndef ljit_h
#define ljit_h

#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"

/* JIT compilation status */
#define JIT_OK        0
#define JIT_FALLBACK  1  /* cannot JIT, use interpreter */
#define JIT_ERR       2

/* Hot loop threshold: compile after this many iterations */
#if !defined(LUA_JIT_THRESHOLD)
#define LUA_JIT_THRESHOLD  100
#endif

/* Maximum JIT code size per function */
#define JIT_MAXCODE   (64 * 1024)

/* Architecture detection */
#if defined(__x86_64__) || defined(_M_X64)
  #define JIT_ARCH_X64   1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define JIT_ARCH_ARM64 1
#elif defined(__i386__) || defined(_M_IX86)
  #define JIT_ARCH_X86   1
#elif defined(__arm__) || defined(_M_ARM)
  #define JIT_ARCH_ARM   1
#endif


/* JIT compiled trace */
typedef struct JitTrace {
  void *code;         /* executable native code pointer (int path) */
  void *fcode;        /* executable native code pointer (float path) */
  size_t code_size;   /* size of allocated int code */
  size_t fcode_size;  /* size of allocated float code */
  int startpc;        /* first PC of the compiled region */
  int endpc;          /* last PC of the compiled region */
} JitTrace;


/* JIT state per Proto */
typedef struct JitState {
  int hotcount;       /* how many times the loop was entered */
  JitTrace *trace;    /* compiled trace (NULL if not yet compiled) */
} JitState;


/* Initialize/shutdown JIT subsystem */
LUAI_FUNC void luaJ_init(lua_State *L);
LUAI_FUNC void luaJ_close(lua_State *L);

/* Try to JIT-compile a for-loop. Returns JIT_OK or JIT_FALLBACK */
LUAI_FUNC int luaJ_compile(lua_State *L, Proto *p, int pc);

/* Execute a JIT-compiled trace. Returns number of results or -1 on fallback */
LUAI_FUNC int luaJ_execute(lua_State *L, CallInfo *ci, JitTrace *trace);

/* Free JIT trace */
LUAI_FUNC void luaJ_freetrace(lua_State *L, JitTrace *trace);


/* ============================================================ */
/* Code emitter abstraction (per-architecture)                   */
/* ============================================================ */

typedef struct JitEmitter {
  unsigned char *code;    /* code buffer */
  size_t pos;             /* current write position */
  size_t capacity;        /* buffer capacity */
} JitEmitter;

/* Emitter functions (implemented per architecture) */
LUAI_FUNC void jit_emit_init(JitEmitter *e, unsigned char *buf, size_t cap);
LUAI_FUNC void jit_emit_prologue(JitEmitter *e);
LUAI_FUNC void jit_emit_epilogue(JitEmitter *e);

/* Emit: reg_a = reg_b + reg_c (integer) */
LUAI_FUNC void jit_emit_addi(JitEmitter *e, int ra, int rb, int rc);
/* Emit: reg_a = reg_b - reg_c */
LUAI_FUNC void jit_emit_subi(JitEmitter *e, int ra, int rb, int rc);
/* Emit: reg_a = reg_b * reg_c */
LUAI_FUNC void jit_emit_muli(JitEmitter *e, int ra, int rb, int rc);
/* Emit: reg_a = immediate integer */
LUAI_FUNC void jit_emit_loadi(JitEmitter *e, int ra, lua_Integer val);
/* Emit: reg_a = reg_b + immediate */
LUAI_FUNC void jit_emit_addimm(JitEmitter *e, int ra, int rb, int imm);
/* Emit: load integer from Lua stack slot */
LUAI_FUNC void jit_emit_load_slot(JitEmitter *e, int cpu_reg, int lua_reg);
/* Emit: store integer to Lua stack slot */
LUAI_FUNC void jit_emit_store_slot(JitEmitter *e, int lua_reg, int cpu_reg);
/* Emit: for-loop numeric (init, limit, step in slots) */
LUAI_FUNC int jit_emit_forloop(JitEmitter *e, int ra, int loop_top);
/* Emit: comparison and conditional branch */
LUAI_FUNC void jit_emit_cmp_jle(JitEmitter *e, int ra, int rb, int *patch);
LUAI_FUNC void jit_emit_patch_jump(JitEmitter *e, int patch_pos);
/* Emit: reg_a = reg_b (float) + reg_c (float) */
LUAI_FUNC void jit_emit_addf(JitEmitter *e, int ra, int rb, int rc);
LUAI_FUNC void jit_emit_subf(JitEmitter *e, int ra, int rb, int rc);
LUAI_FUNC void jit_emit_mulf(JitEmitter *e, int ra, int rb, int rc);
LUAI_FUNC void jit_emit_divf(JitEmitter *e, int ra, int rb, int rc);

/* Pinned register operations (accumulator optimization) */
/* Load Lua slot into pinned register (r14/r7 depending on arch) */
LUAI_FUNC void jit_emit_pin_load(JitEmitter *e, int pin_idx, int lua_reg);
/* Store pinned register back to Lua slot */
LUAI_FUNC void jit_emit_pin_store(JitEmitter *e, int pin_idx, int lua_reg);
/* Pinned add: pin[idx] += cpu_reg (loop var) */
LUAI_FUNC void jit_emit_pin_add_reg(JitEmitter *e, int pin_idx, int cpu_reg);
/* Pinned add immediate: pin[idx] += imm */
LUAI_FUNC void jit_emit_pin_addimm(JitEmitter *e, int pin_idx, int imm);
/* Move pinned to scratch (cpu_reg 0 = rax/r0) */
LUAI_FUNC void jit_emit_pin_to_scratch(JitEmitter *e, int pin_idx);
/* Move scratch to pinned */
LUAI_FUNC void jit_emit_scratch_to_pin(JitEmitter *e, int pin_idx);

/* For-loop control */
/* Load for-loop vars: count, step, idx from Lua slots */
LUAI_FUNC void jit_emit_forloop_load(JitEmitter *e, int ra_for);
/* Emit skip-if-negative check (count < 0 → skip) */
LUAI_FUNC int jit_emit_forloop_skipcheck(JitEmitter *e);
/* Emit loop update: count--, idx += step, store idx, test, branch */
LUAI_FUNC void jit_emit_forloop_update(JitEmitter *e, int ra_for, int loop_top);
/* Store final for-loop values */
LUAI_FUNC void jit_emit_forloop_store(JitEmitter *e, int ra_for);

/* Move loop variable (r9/r6) to scratch */
LUAI_FUNC void jit_emit_loopvar_to_scratch(JitEmitter *e, int cpu_reg);

#endif
