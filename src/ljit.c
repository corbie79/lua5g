/*
** ljit.c - Lightweight JIT compiler for Lua5g
** Compiles hot numeric for-loops to native machine code.
*/

#define ljit_c
#define LUA_CORE

#include "lprefix.h"

#include <string.h>

#include "lua.h"
#include "ljit.h"
#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lvm.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define JIT_HAS_MMAP 1
#endif


/* ============================================================ */
/* Executable memory allocation                                  */
/* ============================================================ */

#if defined(JIT_HAS_MMAP)

static void *jit_alloc_code (size_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}

static void jit_free_code (void *p, size_t size) {
  if (p) munmap(p, size);
}

#else
/* No JIT support on this platform */
static void *jit_alloc_code (size_t size) { (void)size; return NULL; }
static void jit_free_code (void *p, size_t size) { (void)p; (void)size; }
#endif


/* ============================================================ */
/* JIT init/close                                                */
/* ============================================================ */

void luaJ_init (lua_State *L) {
  (void)L;  /* nothing to init yet */
}

void luaJ_close (lua_State *L) {
  (void)L;  /* traces are freed with their protos */
}

void luaJ_freetrace (lua_State *L, JitTrace *trace) {
  if (trace) {
    jit_free_code(trace->code, trace->code_size);
    luaM_free(L, trace);
  }
}


/* ============================================================ */
/* x86-64 code emitter                                           */
/* ============================================================ */

#if defined(JIT_ARCH_X64)

/*
** Register mapping for JIT:
**   rdi = pointer to Lua stack base (StkId)
**   rax, rcx, rdx, r8-r11 = scratch / Lua register values
**
** Lua register R[n] is at: base + n * sizeof(StackValue)
** Each StackValue has: Value (8 bytes) + tt_ (1 byte)
**
** For integer values: value is at offset 0, tt_ at offset 8
** TValuefields = Value value_; lu_byte tt_
** sizeof(TValue) may vary, we use sizeof(StackValue)
*/

/* size of a stack slot (StackValue) */
#define SLOT_SIZE  sizeof(StackValue)
/* offset of value_ in TValue */
#define VAL_OFFSET 0
/* offset of tt_ in TValue */
#define TT_OFFSET  8

/* emit raw bytes */
static void emit_bytes(JitEmitter *e, const unsigned char *b, int n) {
  if (e->pos + (size_t)n <= e->capacity) {
    memcpy(e->code + e->pos, b, (size_t)n);
    e->pos += (size_t)n;
  }
}

static void emit1(JitEmitter *e, unsigned char b) {
  if (e->pos < e->capacity) e->code[e->pos++] = b;
}

static void emit2(JitEmitter *e, unsigned char a, unsigned char b) {
  emit1(e, a); emit1(e, b);
}

static void emit_u32(JitEmitter *e, unsigned int v) {
  emit1(e, (unsigned char)(v & 0xFF));
  emit1(e, (unsigned char)((v >> 8) & 0xFF));
  emit1(e, (unsigned char)((v >> 16) & 0xFF));
  emit1(e, (unsigned char)((v >> 24) & 0xFF));
}

static void emit_u64(JitEmitter *e, unsigned long long v) {
  emit_u32(e, (unsigned int)(v & 0xFFFFFFFF));
  emit_u32(e, (unsigned int)(v >> 32));
}


void jit_emit_init(JitEmitter *e, unsigned char *buf, size_t cap) {
  e->code = buf;
  e->pos = 0;
  e->capacity = cap;
}


/*
** Prologue: save callee-saved registers, set up rdi = stack base
** C calling convention: rdi = 1st arg (stack base pointer)
** typedef int (*JitFunc)(StackValue *base);
*/
void jit_emit_prologue(JitEmitter *e) {
  /* push rbx; push r12; push r13; push r14; push r15 */
  emit1(e, 0x53);                          /* push rbx */
  emit2(e, 0x41, 0x54);                    /* push r12 */
  emit2(e, 0x41, 0x55);                    /* push r13 */
  emit2(e, 0x41, 0x56);                    /* push r14 */
  emit2(e, 0x41, 0x57);                    /* push r15 */
  /* rdi already has base pointer from C call */
}


void jit_emit_epilogue(JitEmitter *e) {
  /* xor eax, eax (return 0 = success) */
  emit2(e, 0x31, 0xC0);
  /* pop r15; pop r14; pop r13; pop r12; pop rbx; ret */
  emit2(e, 0x41, 0x5F);                    /* pop r15 */
  emit2(e, 0x41, 0x5E);                    /* pop r14 */
  emit2(e, 0x41, 0x5D);                    /* pop r13 */
  emit2(e, 0x41, 0x5C);                    /* pop r12 */
  emit1(e, 0x5B);                          /* pop rbx */
  emit1(e, 0xC3);                          /* ret */
}


/*
** Load integer from Lua stack slot R[lua_reg] into CPU register.
** CPU regs: 0=rax, 1=rcx, 2=rdx, 3=rbx, 4=r8...
** mov cpu_reg, [rdi + lua_reg * SLOT_SIZE + VAL_OFFSET]
*/
void jit_emit_load_slot(JitEmitter *e, int cpu_reg, int lua_reg) {
  int offset = lua_reg * (int)SLOT_SIZE + VAL_OFFSET;
  unsigned char rex = 0x48;
  unsigned char modrm_reg;
  /* map cpu_reg to x86-64 register encoding */
  switch (cpu_reg) {
    case 0: modrm_reg = 0; break;          /* rax */
    case 1: modrm_reg = 1; break;          /* rcx */
    case 2: modrm_reg = 2; break;          /* rdx */
    case 3: modrm_reg = 3; break;          /* rbx */
    case 4: rex = 0x4C; modrm_reg = 0; break;  /* r8 */
    case 5: rex = 0x4C; modrm_reg = 1; break;  /* r9 */
    case 6: rex = 0x4C; modrm_reg = 2; break;  /* r10 */
    case 7: rex = 0x4C; modrm_reg = 3; break;  /* r11 */
    default: modrm_reg = 0; break;
  }
  /* REX.W mov reg, [rdi + disp32] */
  emit1(e, rex);
  emit1(e, 0x8B);
  emit1(e, (unsigned char)(0x87 | (modrm_reg << 3)));  /* mod=10, rm=111(rdi) */
  emit_u32(e, (unsigned int)offset);
}


void jit_emit_store_slot(JitEmitter *e, int lua_reg, int cpu_reg) {
  int offset = lua_reg * (int)SLOT_SIZE + VAL_OFFSET;
  unsigned char rex = 0x48;
  unsigned char modrm_reg;
  switch (cpu_reg) {
    case 0: modrm_reg = 0; break;
    case 1: modrm_reg = 1; break;
    case 2: modrm_reg = 2; break;
    case 3: modrm_reg = 3; break;
    case 4: rex = 0x4C; modrm_reg = 0; break;
    case 5: rex = 0x4C; modrm_reg = 1; break;
    case 6: rex = 0x4C; modrm_reg = 2; break;
    case 7: rex = 0x4C; modrm_reg = 3; break;
    default: modrm_reg = 0; break;
  }
  /* REX.W mov [rdi + disp32], reg */
  emit1(e, rex);
  emit1(e, 0x89);
  emit1(e, (unsigned char)(0x87 | (modrm_reg << 3)));
  emit_u32(e, (unsigned int)offset);
}


/* rax = rax + rcx (integer) */
void jit_emit_addi(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  /* add rax, rcx */
  unsigned char buf[] = {0x48, 0x01, 0xC8};  /* add rax, rcx */
  emit_bytes(e, buf, 3);
}

void jit_emit_subi(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  unsigned char buf[] = {0x48, 0x29, 0xC8};  /* sub rax, rcx */
  emit_bytes(e, buf, 3);
}

void jit_emit_muli(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  unsigned char buf[] = {0x48, 0x0F, 0xAF, 0xC1};  /* imul rax, rcx */
  emit_bytes(e, buf, 4);
}

void jit_emit_loadi(JitEmitter *e, int ra, lua_Integer val) {
  (void)ra;
  /* mov rax, imm64 */
  emit2(e, 0x48, 0xB8);
  emit_u64(e, (unsigned long long)val);
}

void jit_emit_addimm(JitEmitter *e, int ra, int rb, int imm) {
  (void)ra; (void)rb;
  /* add rax, imm32 */
  emit2(e, 0x48, 0x05);
  emit_u32(e, (unsigned int)imm);
}


/* Emit comparison and jump: cmp rax, rcx; jle target */
void jit_emit_cmp_jle(JitEmitter *e, int ra, int rb, int *patch) {
  (void)ra; (void)rb;
  /* cmp rax, rcx */
  unsigned char cmp[] = {0x48, 0x39, 0xC8};
  emit_bytes(e, cmp, 3);
  /* jle rel32 (placeholder) */
  emit2(e, 0x0F, 0x8E);
  *patch = (int)e->pos;
  emit_u32(e, 0);  /* placeholder */
}

void jit_emit_patch_jump(JitEmitter *e, int patch_pos) {
  int target = (int)e->pos;
  int rel = target - (patch_pos + 4);  /* relative to after the imm32 */
  e->code[patch_pos] = (unsigned char)(rel & 0xFF);
  e->code[patch_pos + 1] = (unsigned char)((rel >> 8) & 0xFF);
  e->code[patch_pos + 2] = (unsigned char)((rel >> 16) & 0xFF);
  e->code[patch_pos + 3] = (unsigned char)((rel >> 24) & 0xFF);
}

/* Float operations using SSE2 */
void jit_emit_addf(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  /* addsd xmm0, xmm1 */
  unsigned char buf[] = {0xF2, 0x0F, 0x58, 0xC1};
  emit_bytes(e, buf, 4);
}

void jit_emit_subf(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  unsigned char buf[] = {0xF2, 0x0F, 0x5C, 0xC1};
  emit_bytes(e, buf, 4);
}

void jit_emit_mulf(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  unsigned char buf[] = {0xF2, 0x0F, 0x59, 0xC1};
  emit_bytes(e, buf, 4);
}

void jit_emit_divf(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  unsigned char buf[] = {0xF2, 0x0F, 0x5E, 0xC1};
  emit_bytes(e, buf, 4);
}

int jit_emit_forloop(JitEmitter *e, int ra, int loop_top) {
  (void)ra;
  /* This is the jump back instruction */
  /* jmp rel32 */
  emit1(e, 0xE9);
  int rel = loop_top - ((int)e->pos + 4);
  emit_u32(e, (unsigned int)rel);
  return (int)e->pos;
}

#else
/* Stub implementations for non-x86-64 */
void jit_emit_init(JitEmitter *e, unsigned char *buf, size_t cap) {
  e->code = buf; e->pos = 0; e->capacity = cap;
}
void jit_emit_prologue(JitEmitter *e) { (void)e; }
void jit_emit_epilogue(JitEmitter *e) { (void)e; }
void jit_emit_load_slot(JitEmitter *e, int r, int s) { (void)e;(void)r;(void)s; }
void jit_emit_store_slot(JitEmitter *e, int s, int r) { (void)e;(void)r;(void)s; }
void jit_emit_addi(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_subi(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_muli(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_loadi(JitEmitter *e, int a, lua_Integer v) { (void)e;(void)a;(void)v; }
void jit_emit_addimm(JitEmitter *e, int a, int b, int i) { (void)e;(void)a;(void)b;(void)i; }
void jit_emit_cmp_jle(JitEmitter *e, int a, int b, int *p) { (void)e;(void)a;(void)b;(void)p; }
void jit_emit_patch_jump(JitEmitter *e, int p) { (void)e;(void)p; }
void jit_emit_addf(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_subf(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_mulf(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
void jit_emit_divf(JitEmitter *e, int a, int b, int c) { (void)e;(void)a;(void)b;(void)c; }
int jit_emit_forloop(JitEmitter *e, int a, int t) { (void)e;(void)a;(void)t; return 0; }
#endif


/* ============================================================ */
/* JIT Compiler: bytecode -> native code                         */
/* ============================================================ */

/*
** Compile a numeric for-loop to native code.
** The loop starts at 'startpc' (OP_FORPREP) and ends at the matching
** OP_FORLOOP. Only compiles loops with pure integer arithmetic.
**
** Compiled function signature:
**   int jit_func(StackValue *base)
** Returns 0 on success.
*/
int luaJ_compile (lua_State *L, Proto *p, int pc) {
#if !defined(JIT_ARCH_X64)
  (void)L; (void)p; (void)pc;
  return JIT_FALLBACK;
#else
  Instruction *code = p->code;
  Instruction forprep = code[pc];
  int ra_for = GETARG_A(forprep);  /* base register of for loop */
  int loop_end_pc;
  int i;
  JitEmitter em;
  unsigned char *buf;
  JitTrace *trace;

  /* Verify this is OP_FORPREP */
  if (GET_OPCODE(forprep) != OP_FORPREP)
    return JIT_FALLBACK;

  /* Find the OP_FORLOOP */
  loop_end_pc = pc + 1 + GETARG_Bx(forprep);
  if (loop_end_pc >= p->sizecode)
    return JIT_FALLBACK;
  if (GET_OPCODE(code[loop_end_pc]) != OP_FORLOOP)
    return JIT_FALLBACK;

  /* Scan loop body: only allow simple integer ops */
  for (i = pc + 1; i < loop_end_pc; i++) {
    OpCode op = GET_OPCODE(code[i]);
    switch (op) {
      case OP_MOVE: case OP_LOADI: case OP_LOADK:
      case OP_ADD: case OP_SUB: case OP_MUL:
      case OP_ADDI: case OP_ADDK: case OP_SUBK: case OP_MULK:
      case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
        break;  /* OK, can compile */
      default:
        return JIT_FALLBACK;  /* unsupported opcode */
    }
  }

  /* Allocate executable memory */
  buf = (unsigned char *)jit_alloc_code(JIT_MAXCODE);
  if (buf == NULL)
    return JIT_ERR;

  jit_emit_init(&em, buf, JIT_MAXCODE);

  /* === Emit prologue === */
  jit_emit_prologue(&em);

  /*
  ** For-loop registers (Lua 5.5):
  **   R[A]   = internal index (counter)
  **   R[A+1] = limit
  **   R[A+2] = step
  **   R[A+3] = external index (visible 'i')
  **
  ** OP_FORPREP: check values, if not to run then skip
  ** OP_FORLOOP: index += step; if index <= limit then continue
  **
  ** We compile as:
  **   load index, limit, step from stack
  **   loop_top:
  **     <body>
  **     index += step
  **     if index <= limit: goto loop_top
  **   store index back
  */

  /*
  ** After forprep, stack layout for integer loops:
  **   R[A]   = count (remaining iterations, unsigned)
  **   R[A+1] = step
  **   R[A+2] = control variable (current i)
  **
  ** OP_FORLOOP: if count > 0: count--; i += step; jump back
  **
  ** JIT strategy: load count/step/i into registers, run body,
  ** update and loop entirely in native code.
  **
  ** Register mapping:
  **   rbx = count, r8 = step, r9 = control variable (i)
  */
  jit_emit_load_slot(&em, 3, ra_for);      /* rbx = R[A] (count) */
  jit_emit_load_slot(&em, 4, ra_for + 1);  /* r8 = R[A+1] (step) */
  jit_emit_load_slot(&em, 5, ra_for + 2);  /* r9 = R[A+2] (control/i) */

  /* === Check: if count < 0, skip loop (forprep sets -1 if skip) === */
  {
    unsigned char buf2[] = {0x48, 0x85, 0xDB};  /* test rbx, rbx */
    emit_bytes(&em, buf2, 3);
  }
  emit2(&em, 0x0F, 0x88);  /* js rel32 (jump if sign/negative) */
  int skip_patch = (int)em.pos;
  emit_u32(&em, 0);

  /* === Loop top === */
  int loop_top = (int)em.pos;

  /* === Compile loop body === */
  for (i = pc + 1; i < loop_end_pc; i++) {
    Instruction inst = code[i];
    OpCode op = GET_OPCODE(inst);
    int a = GETARG_A(inst);

    switch (op) {
      case OP_ADDI: {
        int b = GETARG_B(inst);
        int sc = GETARG_sC(inst);
        jit_emit_load_slot(&em, 0, b);
        jit_emit_addimm(&em, 0, 0, sc);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_ADD: {
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        jit_emit_load_slot(&em, 0, b);
        jit_emit_load_slot(&em, 1, c);
        jit_emit_addi(&em, 0, 0, 1);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_SUB: {
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        jit_emit_load_slot(&em, 0, b);
        jit_emit_load_slot(&em, 1, c);
        jit_emit_subi(&em, 0, 0, 1);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_MUL: {
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        jit_emit_load_slot(&em, 0, b);
        jit_emit_load_slot(&em, 1, c);
        jit_emit_muli(&em, 0, 0, 1);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_MOVE: {
        int b = GETARG_B(inst);
        jit_emit_load_slot(&em, 0, b);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_LOADI: {
        lua_Integer val = GETARG_sBx(inst);
        jit_emit_loadi(&em, 0, val);
        jit_emit_store_slot(&em, a, 0);
        break;
      }
      case OP_LOADK: {
        /* R[A] = K[Bx] - load constant (must be integer) */
        int bx = GETARG_Bx(inst);
        if (bx < p->sizek && ttisinteger(&p->k[bx])) {
          lua_Integer val = ivalue(&p->k[bx]);
          jit_emit_loadi(&em, 0, val);
          jit_emit_store_slot(&em, a, 0);
        }
        break;
      }
      case OP_ADDK: {
        /* R[A] = R[B] + K[C] */
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        if (c < p->sizek && ttisinteger(&p->k[c])) {
          jit_emit_load_slot(&em, 0, b);
          lua_Integer kval = ivalue(&p->k[c]);
          /* add rax, imm32 */
          jit_emit_addimm(&em, 0, 0, (int)kval);
          jit_emit_store_slot(&em, a, 0);
        }
        break;
      }
      case OP_SUBK: {
        /* R[A] = R[B] - K[C] */
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        if (c < p->sizek && ttisinteger(&p->k[c])) {
          jit_emit_load_slot(&em, 0, b);
          lua_Integer kval = ivalue(&p->k[c]);
          /* sub rax, imm32 */
          emit2(&em, 0x48, 0x2D);
          emit_u32(&em, (unsigned int)(int)kval);
          jit_emit_store_slot(&em, a, 0);
        }
        break;
      }
      case OP_MULK: {
        /* R[A] = R[B] * K[C] */
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        if (c < p->sizek && ttisinteger(&p->k[c])) {
          jit_emit_load_slot(&em, 0, b);
          lua_Integer kval = ivalue(&p->k[c]);
          jit_emit_loadi(&em, 1, kval);  /* rcx = constant */
          jit_emit_muli(&em, 0, 0, 1);   /* rax *= rcx */
          jit_emit_store_slot(&em, a, 0);
        }
        break;
      }
      case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
        break;  /* skip metamethod markers */
      default:
        break;
    }
  }

  /* === For-loop update === */
  /* count-- */
  {
    unsigned char buf2[] = {0x48, 0xFF, 0xCB};  /* dec rbx */
    emit_bytes(&em, buf2, 3);
  }
  /* i += step: r9 += r8 */
  {
    unsigned char buf2[] = {0x4D, 0x01, 0xC1};  /* add r9, r8 */
    emit_bytes(&em, buf2, 3);
  }
  /* store updated i to R[A+2] for use by body instructions */
  jit_emit_store_slot(&em, ra_for + 2, 5);  /* R[A+2] = r9 */

  /* test rbx, rbx; jns loop_top (loop while count >= 0, signed) */
  {
    unsigned char buf2[] = {0x48, 0x85, 0xDB};  /* test rbx, rbx */
    emit_bytes(&em, buf2, 3);
  }
  {
    int rel = loop_top - ((int)em.pos + 6);
    emit2(&em, 0x0F, 0x89);  /* jns rel32 (jump if not sign = >= 0) */
    emit_u32(&em, (unsigned int)rel);
  }

  /* Patch skip jump */
  jit_emit_patch_jump(&em, skip_patch);

  /* === Store final values back to Lua stack === */
  jit_emit_store_slot(&em, ra_for, 3);      /* R[A] = count (should be 0) */
  jit_emit_store_slot(&em, ra_for + 2, 5);  /* R[A+2] = final i */

  /* === Epilogue === */
  jit_emit_epilogue(&em);

  /* Create trace */
  trace = luaM_new(L, JitTrace);
  trace->code = buf;
  trace->code_size = em.pos;
  trace->startpc = pc;
  trace->endpc = loop_end_pc;

  /* Store trace in Proto */
  if (p->jit != NULL)
    luaJ_freetrace(L, p->jit);
  p->jit = trace;

  return JIT_OK;
#endif
}


/*
** Execute a JIT-compiled trace.
** Returns 0 on success.
*/
int luaJ_execute (lua_State *L, CallInfo *ci, JitTrace *trace) {
#if !defined(JIT_ARCH_X64)
  (void)L; (void)ci; (void)trace;
  return -1;
#else
  typedef int (*JitFunc)(void *base);
  JitFunc fn = (JitFunc)trace->code;
  StkId base = ci->func.p + 1;
  (void)L;
  return fn(base);
#endif
}
