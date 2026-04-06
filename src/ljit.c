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
    if (trace->code) jit_free_code(trace->code, trace->code_size);
    if (trace->fcode) jit_free_code(trace->fcode, trace->fcode_size);
    luaM_free(L, trace);
  }
}


/* ============================================================ */
/* x86-64 code emitter                                           */
/* ============================================================ */

/* Common: stack slot size (used by all backends and compiler) */
#define SLOT_SIZE  sizeof(StackValue)

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
** typedef int (*JitFunc)(StackValue *base);
*/
void jit_emit_prologue(JitEmitter *e) {
  emit1(e, 0x53);                          /* push rbx */
  emit2(e, 0x41, 0x54);                    /* push r12 */
  emit2(e, 0x41, 0x55);                    /* push r13 */
  emit2(e, 0x41, 0x56);                    /* push r14 */
  emit2(e, 0x41, 0x57);                    /* push r15 */
}


void jit_emit_epilogue(JitEmitter *e) {
  /* xor eax, eax (return 0 = success) */
  emit2(e, 0x31, 0xC0);
  /* pop r15; pop r14; pop r13; pop r12; pop rbx; ret */
  emit2(e, 0x41, 0x5F);
  emit2(e, 0x41, 0x5E);
  emit2(e, 0x41, 0x5D);
  emit2(e, 0x41, 0x5C);
  emit1(e, 0x5B);
  emit1(e, 0xC3);
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

/* === x86-64 pinned register ops (r14 = pin[0], r15 = pin[1]) === */

void jit_emit_pin_load(JitEmitter *e, int pin_idx, int lua_reg) {
  int offset = lua_reg * (int)SLOT_SIZE;
  if (pin_idx == 0) {
    emit2(e, 0x4C, 0x8B); emit1(e, 0xB7);  /* mov r14, [rdi+disp] */
  } else {
    emit2(e, 0x4C, 0x8B); emit1(e, 0xBF);  /* mov r15, [rdi+disp] */
  }
  emit_u32(e, (unsigned int)offset);
}

void jit_emit_pin_store(JitEmitter *e, int pin_idx, int lua_reg) {
  int offset = lua_reg * (int)SLOT_SIZE;
  if (pin_idx == 0) {
    emit2(e, 0x4C, 0x89); emit1(e, 0xB7);  /* mov [rdi+disp], r14 */
  } else {
    emit2(e, 0x4C, 0x89); emit1(e, 0xBF);
  }
  emit_u32(e, (unsigned int)offset);
}

void jit_emit_pin_add_reg(JitEmitter *e, int pin_idx, int cpu_reg) {
  (void)cpu_reg;  /* always r9 for loop var */
  if (pin_idx == 0) {
    unsigned char b[] = {0x4D, 0x01, 0xCE};  /* add r14, r9 */
    emit_bytes(e, b, 3);
  }
}

void jit_emit_pin_addimm(JitEmitter *e, int pin_idx, int imm) {
  if (pin_idx == 0) {
    emit2(e, 0x49, 0x81); emit1(e, 0xC6);  /* add r14, imm32 */
    emit_u32(e, (unsigned int)imm);
  }
}

void jit_emit_pin_to_scratch(JitEmitter *e, int pin_idx) {
  if (pin_idx == 0) {
    emit2(e, 0x4C, 0x89); emit1(e, 0xF0);  /* mov rax, r14 */
  }
}

void jit_emit_scratch_to_pin(JitEmitter *e, int pin_idx) {
  if (pin_idx == 0) {
    emit2(e, 0x49, 0x89); emit1(e, 0xC6);  /* mov r14, rax */
  }
}

void jit_emit_loopvar_to_scratch(JitEmitter *e, int cpu_reg) {
  if (cpu_reg == 1) {  /* r9 → rcx */
    emit2(e, 0x4C, 0x89); emit1(e, 0xC9);  /* mov rcx, r9 */
  }
}

/* === x86-64 for-loop control === */

void jit_emit_forloop_load(JitEmitter *e, int ra_for) {
  jit_emit_load_slot(e, 3, ra_for);      /* rbx = count */
  jit_emit_load_slot(e, 4, ra_for + 1);  /* r8 = step */
  jit_emit_load_slot(e, 5, ra_for + 2);  /* r9 = idx */
}

int jit_emit_forloop_skipcheck(JitEmitter *e) {
  unsigned char b[] = {0x48, 0x85, 0xDB};  /* test rbx, rbx */
  emit_bytes(e, b, 3);
  emit2(e, 0x0F, 0x88);  /* js rel32 */
  int patch = (int)e->pos;
  emit_u32(e, 0);
  return patch;
}

void jit_emit_forloop_update(JitEmitter *e, int ra_for, int loop_top) {
  /* dec rbx */
  unsigned char dec[] = {0x48, 0xFF, 0xCB};
  emit_bytes(e, dec, 3);
  /* add r9, r8 */
  unsigned char add[] = {0x4D, 0x01, 0xC1};
  emit_bytes(e, add, 3);
  /* store idx: R[A+2] = r9 */
  jit_emit_store_slot(e, ra_for + 2, 5);
  /* test rbx, rbx; jns loop_top */
  unsigned char test[] = {0x48, 0x85, 0xDB};
  emit_bytes(e, test, 3);
  int rel = loop_top - ((int)e->pos + 6);
  emit2(e, 0x0F, 0x89);  /* jns rel32 */
  emit_u32(e, (unsigned int)rel);
}

void jit_emit_forloop_store(JitEmitter *e, int ra_for) {
  jit_emit_store_slot(e, ra_for, 3);      /* R[A] = count */
  jit_emit_store_slot(e, ra_for + 2, 5);  /* R[A+2] = final idx */
}


#elif defined(JIT_ARCH_ARM)

/* ============================================================ */
/* ARMv7 code emitter (ARM mode, 32-bit instructions)            */
/* ============================================================ */

/*
** ARMv7 register mapping:
**   r0 = arg: pointer to Lua stack base
**   r4-r11 = callee-saved (we use them for Lua values)
**   r4 = rbx equivalent (count)
**   r5 = r8 equivalent (step)
**   r6 = r9 equivalent (control variable i)
**   r7 = pinned accumulator #1
**   r14 = link register (saved in prologue)
**
** StackValue on ARMv7 (32-bit):
**   sizeof(TValue) = 12 (8 bytes value + 4 bytes tag on 32-bit?)
**   Actually on 32-bit: Value = 8 bytes (union), tt_ = 1 byte,
**   but StackValue might be padded. We use sizeof(StackValue).
**
** For ARMv7 with 32-bit lua_Integer: value is at offset 0, 4 bytes.
** For ARMv7 with 64-bit lua_Number: value is at offset 0, 8 bytes.
**
** ARM instruction encoding: little-endian 32-bit words
*/

#define SLOT_SIZE  sizeof(StackValue)

/* emit 32-bit ARM instruction (little-endian) */
static void arm_emit32(JitEmitter *e, unsigned int inst) {
  if (e->pos + 4 <= e->capacity) {
    e->code[e->pos++] = (unsigned char)(inst & 0xFF);
    e->code[e->pos++] = (unsigned char)((inst >> 8) & 0xFF);
    e->code[e->pos++] = (unsigned char)((inst >> 16) & 0xFF);
    e->code[e->pos++] = (unsigned char)((inst >> 24) & 0xFF);
  }
}

/* ARM condition codes */
#define ARM_AL  0xE   /* always */
#define ARM_EQ  0x0
#define ARM_NE  0x1
#define ARM_GE  0xA
#define ARM_LT  0xB
#define ARM_GT  0xC
#define ARM_LE  0xD
#define ARM_MI  0x4   /* minus/negative */
#define ARM_PL  0x5   /* plus/positive or zero */

/* ARM data processing: cond|00|I|opcode|S|Rn|Rd|operand2 */
#define ARM_DP(cond, op, s, rn, rd, op2) \
  (((cond)<<28) | (0<<26) | ((op)<<21) | ((s)<<20) | ((rn)<<16) | ((rd)<<12) | (op2))

/* ARM opcodes */
#define ARM_ADD  4
#define ARM_SUB  2
#define ARM_MOV  13
#define ARM_CMP  10
#define ARM_MUL_OP  0  /* special encoding */

/* LDR Rd, [Rn, #offset] */
#define ARM_LDR(cond, rd, rn, off) \
  (((cond)<<28) | (0x05<<24) | (1<<23) | ((rn)<<16) | ((rd)<<12) | ((off) & 0xFFF))

/* STR Rd, [Rn, #offset] */
#define ARM_STR(cond, rd, rn, off) \
  (((cond)<<28) | (0x05<<24) | (0<<24) | (1<<23) | ((rn)<<16) | ((rd)<<12) | ((off) & 0xFFF))

/* Branch: cond|101|L|offset (24-bit signed, in words) */
#define ARM_B(cond, offset) \
  (((cond)<<28) | (0xA<<24) | ((offset) & 0x00FFFFFF))


void jit_emit_init(JitEmitter *e, unsigned char *buf, size_t cap) {
  e->code = buf; e->pos = 0; e->capacity = cap;
}

/*
** Prologue: push {r4-r11, lr}
** r0 = base pointer (first arg, AAPCS)
*/
void jit_emit_prologue(JitEmitter *e) {
  /* PUSH {r4-r11, lr} = STMFD sp!, {r4-r11, r14} */
  arm_emit32(e, 0xE92D4FF0);  /* push {r4-r11, lr} */
  /* mov r11, r0  (save base pointer in r11) */
  arm_emit32(e, ARM_DP(ARM_AL, ARM_MOV, 0, 0, 11, 0));  /* mov r11, r0 */
}

void jit_emit_epilogue(JitEmitter *e) {
  /* mov r0, #0 (return 0) */
  arm_emit32(e, ARM_DP(ARM_AL, ARM_MOV, 0, 0, 0, 0));
  /* POP {r4-r11, pc} = LDMFD sp!, {r4-r11, r15} */
  arm_emit32(e, 0xE8BD8FF0);  /* pop {r4-r11, pc} */
}

/*
** Load integer from Lua stack slot R[lua_reg] into ARM register.
** ARM regs: r0-r3=scratch, r4-r10=mapped
** LDR rd, [r11, #lua_reg * SLOT_SIZE]
*/
void jit_emit_load_slot(JitEmitter *e, int cpu_reg, int lua_reg) {
  int rd = (cpu_reg < 4) ? cpu_reg : cpu_reg;  /* map directly */
  int offset = lua_reg * (int)SLOT_SIZE;
  if (offset < 4096) {
    arm_emit32(e, ARM_LDR(ARM_AL, rd, 11, offset));
  }
}

void jit_emit_store_slot(JitEmitter *e, int lua_reg, int cpu_reg) {
  int rd = cpu_reg;
  int offset = lua_reg * (int)SLOT_SIZE;
  if (offset < 4096) {
    /* STR rd, [r11, #offset] */
    arm_emit32(e, ((ARM_AL)<<28) | (0x05<<24) | (1<<23) | (11<<16) | (rd<<12) | (offset & 0xFFF));
  }
}

/* ADD rd, rn, rm */
void jit_emit_addi(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  /* add r0, r0, r1 */
  arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, 0, 0, 1));
}

void jit_emit_subi(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  arm_emit32(e, ARM_DP(ARM_AL, ARM_SUB, 0, 0, 0, 1));
}

void jit_emit_muli(JitEmitter *e, int ra, int rb, int rc) {
  (void)ra; (void)rb; (void)rc;
  /* MUL r0, r0, r1: cond|000000|AS|Rd|0000|Rs|1001|Rm */
  arm_emit32(e, (ARM_AL<<28) | (0<<16) | (1<<8) | (0x90) | 0);
}

void jit_emit_loadi(JitEmitter *e, int ra, lua_Integer v) {
  (void)ra;
  /* MOV r0, #imm (for small values) + MOVT for high bits */
  unsigned int uv = (unsigned int)v;
  /* MOVW r0, #imm16 (ARMv7) */
  arm_emit32(e, (ARM_AL<<28) | (0x30<<20) | (0<<12) | ((uv & 0xF000)<<4) | (uv & 0xFFF));
  if (uv > 0xFFFF) {
    /* MOVT r0, #imm16 */
    unsigned int hi = uv >> 16;
    arm_emit32(e, (ARM_AL<<28) | (0x34<<20) | (0<<12) | ((hi & 0xF000)<<4) | (hi & 0xFFF));
  }
}

void jit_emit_addimm(JitEmitter *e, int ra, int rb, int imm) {
  (void)ra; (void)rb;
  /* ADD r0, r0, #imm (if imm fits in 8-bit rotated) */
  if (imm >= 0 && imm < 256)
    arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, 0, 0, (1<<25) | imm));
  else {
    /* load imm to r1, then add */
    unsigned int uv = (unsigned int)imm;
    arm_emit32(e, (ARM_AL<<28) | (0x30<<20) | (1<<12) | ((uv & 0xF000)<<4) | (uv & 0xFFF));
    arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, 0, 0, 1));
  }
}

void jit_emit_cmp_jle(JitEmitter *e, int ra, int rb, int *patch) {
  (void)ra; (void)rb;
  /* CMP r0, r1 */
  arm_emit32(e, ARM_DP(ARM_AL, ARM_CMP, 1, 0, 0, 1));
  /* BLE offset (placeholder) */
  *patch = (int)e->pos;
  arm_emit32(e, ARM_B(ARM_LE, 0));
}

void jit_emit_patch_jump(JitEmitter *e, int patch_pos) {
  int target = (int)e->pos;
  int rel = ((target - patch_pos - 8) >> 2) & 0x00FFFFFF;  /* ARM: PC+8, words */
  unsigned int inst = ARM_B(ARM_LE, rel);
  e->code[patch_pos] = (unsigned char)(inst & 0xFF);
  e->code[patch_pos+1] = (unsigned char)((inst >> 8) & 0xFF);
  e->code[patch_pos+2] = (unsigned char)((inst >> 16) & 0xFF);
  e->code[patch_pos+3] = (unsigned char)((inst >> 24) & 0xFF);
}

/* VFP double precision */
void jit_emit_addf(JitEmitter *e, int a, int b, int c) {
  (void)a; (void)b; (void)c;
  /* VADD.F64 d0, d0, d1 */
  arm_emit32(e, 0xEE300B01);
}
void jit_emit_subf(JitEmitter *e, int a, int b, int c) {
  (void)a; (void)b; (void)c;
  arm_emit32(e, 0xEE300B41);  /* VSUB.F64 d0, d0, d1 */
}
void jit_emit_mulf(JitEmitter *e, int a, int b, int c) {
  (void)a; (void)b; (void)c;
  arm_emit32(e, 0xEE200B01);  /* VMUL.F64 d0, d0, d1 */
}
void jit_emit_divf(JitEmitter *e, int a, int b, int c) {
  (void)a; (void)b; (void)c;
  arm_emit32(e, 0xEE800B01);  /* VDIV.F64 d0, d0, d1 */
}
int jit_emit_forloop(JitEmitter *e, int a, int t) {
  (void)a;
  int rel = ((t - (int)e->pos - 8) >> 2) & 0x00FFFFFF;
  arm_emit32(e, ARM_B(ARM_AL, rel));  /* B loop_top */
  return (int)e->pos;
}

/* ARMv7 pinned + forloop ops */
void jit_emit_pin_load(JitEmitter *e, int pin_idx, int lua_reg) {
  int rd = (pin_idx == 0) ? 7 : 8;
  int offset = lua_reg * (int)SLOT_SIZE;
  if (offset < 4096) arm_emit32(e, ARM_LDR(ARM_AL, rd, 11, offset));
}
void jit_emit_pin_store(JitEmitter *e, int pin_idx, int lua_reg) {
  int rd = (pin_idx == 0) ? 7 : 8;
  int offset = lua_reg * (int)SLOT_SIZE;
  if (offset < 4096) arm_emit32(e, ((ARM_AL)<<28)|(0x05<<24)|(1<<23)|(11<<16)|(rd<<12)|(offset&0xFFF));
}
void jit_emit_pin_add_reg(JitEmitter *e, int pin_idx, int c) {
  (void)c; int rd=(pin_idx==0)?7:8;
  arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, rd, rd, 6));
}
void jit_emit_pin_addimm(JitEmitter *e, int pin_idx, int imm) {
  int rd=(pin_idx==0)?7:8;
  if (imm>=0 && imm<256) arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, rd, rd, (1<<25)|imm));
}
void jit_emit_pin_to_scratch(JitEmitter *e, int p) {
  arm_emit32(e, ARM_DP(ARM_AL, ARM_MOV, 0, 0, 0, (p==0)?7:8));
}
void jit_emit_scratch_to_pin(JitEmitter *e, int p) {
  arm_emit32(e, ARM_DP(ARM_AL, ARM_MOV, 0, 0, (p==0)?7:8, 0));
}
void jit_emit_loopvar_to_scratch(JitEmitter *e, int c) {
  (void)c; arm_emit32(e, ARM_DP(ARM_AL, ARM_MOV, 0, 0, 1, 6));
}
void jit_emit_forloop_load(JitEmitter *e, int ra) {
  jit_emit_load_slot(e,4,ra); jit_emit_load_slot(e,5,ra+1); jit_emit_load_slot(e,6,ra+2);
}
int jit_emit_forloop_skipcheck(JitEmitter *e) {
  arm_emit32(e, ARM_DP(ARM_AL, ARM_CMP, 1, 4, 0, (1<<25)|0));
  int p=(int)e->pos; arm_emit32(e, ARM_B(ARM_MI, 0)); return p;
}
void jit_emit_forloop_update(JitEmitter *e, int ra, int lt) {
  arm_emit32(e, ARM_DP(ARM_AL, ARM_SUB, 1, 4, 4, (1<<25)|1));
  arm_emit32(e, ARM_DP(ARM_AL, ARM_ADD, 0, 6, 6, 5));
  jit_emit_store_slot(e, ra+2, 6);
  arm_emit32(e, ARM_DP(ARM_AL, ARM_CMP, 1, 4, 0, (1<<25)|0));
  int rel=((lt-(int)e->pos-8)>>2)&0x00FFFFFF;
  arm_emit32(e, ARM_B(ARM_PL, rel));
}
void jit_emit_forloop_store(JitEmitter *e, int ra) {
  jit_emit_store_slot(e,ra,4); jit_emit_store_slot(e,ra+2,6);
}

#else
/* No JIT support on this platform */
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
void jit_emit_pin_load(JitEmitter *e, int p, int r) { (void)e;(void)p;(void)r; }
void jit_emit_pin_store(JitEmitter *e, int p, int r) { (void)e;(void)p;(void)r; }
void jit_emit_pin_add_reg(JitEmitter *e, int p, int c) { (void)e;(void)p;(void)c; }
void jit_emit_pin_addimm(JitEmitter *e, int p, int i) { (void)e;(void)p;(void)i; }
void jit_emit_pin_to_scratch(JitEmitter *e, int p) { (void)e;(void)p; }
void jit_emit_scratch_to_pin(JitEmitter *e, int p) { (void)e;(void)p; }
void jit_emit_loopvar_to_scratch(JitEmitter *e, int c) { (void)e;(void)c; }
void jit_emit_forloop_load(JitEmitter *e, int r) { (void)e;(void)r; }
int jit_emit_forloop_skipcheck(JitEmitter *e) { (void)e; return 0; }
void jit_emit_forloop_update(JitEmitter *e, int r, int t) { (void)e;(void)r;(void)t; }
void jit_emit_forloop_store(JitEmitter *e, int r) { (void)e;(void)r; }
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
      case OP_MOVE: case OP_LOADI: case OP_LOADK: case OP_LOADF:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV:
      case OP_ADDI: case OP_ADDK: case OP_SUBK: case OP_MULK:
      case OP_DIVK: case OP_IDIVK:
      /* Table ops via C helper: overhead ≈ interpreter, no real gain.
         GETFIELD/GETI/SETFIELD/SETI/CALL all fall back to interpreter. */
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
  /*
  ** Register promotion: scan body to find accumulator slots.
  ** A slot that is both read and written in the same instruction
  ** (e.g., ADD R[1], R[1], R[4]) is an accumulator candidate.
  ** Pin up to 2 accumulators to r12, r13 (callee-saved).
  ** The loop variable R[A+2] is already pinned to r9.
  */
  #define MAX_PINNED 2
  int pinned_slot[MAX_PINNED];   /* Lua register index */
  int pinned_cpureg[MAX_PINNED]; /* CPU register: 6=r12, 7=r13 (mapped) */
  int npinned = 0;
  /* r12 = cpu_reg 6, r13 = cpu_reg 7 in our mapping */
  /* Actually: let's use r14=index 14, r15=index 15 approach differently */
  /* Simpler: pin accumulators directly. Scan: */
  {
    int slot_rw[256] = {0};  /* bitmask: 1=read, 2=write */
    for (i = pc + 1; i < loop_end_pc; i++) {
      OpCode op = GET_OPCODE(code[i]);
      int a = GETARG_A(code[i]);
      switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL: {
          int b = GETARG_B(code[i]);
          int c = GETARG_C(code[i]);
          slot_rw[b] |= 1; slot_rw[c] |= 1; slot_rw[a] |= 2;
          break;
        }
        case OP_ADDI: case OP_ADDK: case OP_SUBK: case OP_MULK: {
          int b = GETARG_B(code[i]);
          slot_rw[b] |= 1; slot_rw[a] |= 2;
          break;
        }
        default: break;
      }
    }
    /* find slots that are both read AND written (accumulators) */
    for (i = 0; i < 256 && npinned < MAX_PINNED; i++) {
      if (i == ra_for || i == ra_for + 1 || i == ra_for + 2)
        continue;  /* skip for-loop control slots (already handled) */
      if (slot_rw[i] == 3) {  /* both read and written */
        pinned_slot[npinned] = i;
        npinned++;
      }
    }
  }

  /* Load for-loop control vars into arch-specific registers */
  jit_emit_forloop_load(&em, ra_for);

  /* Load pinned accumulators */
  if (npinned >= 1)
    jit_emit_pin_load(&em, 0, pinned_slot[0]);
  if (npinned >= 2)
    jit_emit_pin_load(&em, 1, pinned_slot[1]);

  /* === Check: if count < 0, skip loop === */
  int skip_patch = jit_emit_forloop_skipcheck(&em);

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
        if (npinned >= 1 && a == pinned_slot[0] && b == pinned_slot[0]) {
          jit_emit_pin_addimm(&em, 0, sc);
        }
        else {
          if (npinned >= 1 && b == pinned_slot[0])
            jit_emit_pin_to_scratch(&em, 0);
          else
            jit_emit_load_slot(&em, 0, b);
          jit_emit_addimm(&em, 0, 0, sc);
          if (npinned >= 1 && a == pinned_slot[0])
            jit_emit_scratch_to_pin(&em, 0);
          else
            jit_emit_store_slot(&em, a, 0);
        }
        break;
      }
      case OP_ADD: {
        int b = GETARG_B(inst);
        int c = GETARG_C(inst);
        /* Fast: accumulator += loop_var (pure register) */
        if (npinned >= 1 && a == pinned_slot[0] && b == pinned_slot[0]
            && c == ra_for + 2) {
          jit_emit_pin_add_reg(&em, 0, 5);  /* pin[0] += loopvar */
        }
        else if (npinned >= 1 && a == pinned_slot[0] && c == pinned_slot[0]
                 && b == ra_for + 2) {
          jit_emit_pin_add_reg(&em, 0, 5);  /* commutative */
        }
        else {
          /* generic path */
          if (b == ra_for + 2)
            jit_emit_loopvar_to_scratch(&em, 0);
          else if (npinned >= 1 && b == pinned_slot[0])
            jit_emit_pin_to_scratch(&em, 0);
          else
            jit_emit_load_slot(&em, 0, b);
          if (c == ra_for + 2)
            jit_emit_loopvar_to_scratch(&em, 1);
          else if (npinned >= 1 && c == pinned_slot[0])
            jit_emit_pin_to_scratch(&em, 0);  /* to scratch reg 0 */
          else
            jit_emit_load_slot(&em, 1, c);
          jit_emit_addi(&em, 0, 0, 1);
          if (npinned >= 1 && a == pinned_slot[0])
            jit_emit_scratch_to_pin(&em, 0);
          else
            jit_emit_store_slot(&em, a, 0);
        }
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
        break;
      default:
        break;
    }
  }

  /* Flush pinned accumulators to stack before for-loop update
     (GETFIELD/SETFIELD reads from stack) */
  if (npinned >= 1) {
    emit2(&em, 0x4C, 0x89);
    emit1(&em, 0xB7);
    emit_u32(&em, (unsigned int)(pinned_slot[0] * (int)SLOT_SIZE));
  }

  /* === For-loop update (architecture-independent) === */
  jit_emit_forloop_update(&em, ra_for, loop_top);

  /* Patch skip jump */
  jit_emit_patch_jump(&em, skip_patch);

  /* === Store final values back to Lua stack === */
  jit_emit_forloop_store(&em, ra_for);
  if (npinned >= 1)
    jit_emit_pin_store(&em, 0, pinned_slot[0]);
  if (npinned >= 2)
    jit_emit_pin_store(&em, 1, pinned_slot[1]);

  /* === Epilogue === */
  jit_emit_epilogue(&em);

  /* ============================================================ */
  /* Generate float version (SSE2)                                */
  /* ============================================================ */
  /*
  ** Float for-loop after forprep:
  **   R[A]   = limit (double)
  **   R[A+1] = step (double)
  **   R[A+2] = control variable (double, starts at init)
  **
  ** Float loop: idx += step; if (step>0 ? idx<=limit : limit<=idx) continue
  ** For simplicity, only handle step > 0 ascending loops.
  **
  ** SSE2 register mapping:
  **   xmm0 = idx (control), xmm1 = step, xmm2 = limit
  **   xmm3-xmm7 = scratch for body ops
  **
  ** Stack slot value_ offset for float: same as int (offset 0, 8 bytes)
  */
  unsigned char *fbuf = (unsigned char *)jit_alloc_code(JIT_MAXCODE);
  size_t fcode_size = 0;
  if (fbuf != NULL) {
    JitEmitter fem;
    jit_emit_init(&fem, fbuf, JIT_MAXCODE);

    /* prologue */
    jit_emit_prologue(&fem);

    /* load float loop vars from stack:
       xmm2 = R[A].value_ (limit)
       xmm1 = R[A+1].value_ (step)
       xmm0 = R[A+2].value_ (control/idx) */
    #define FEMIT(b, n) emit_bytes(&fem, (const unsigned char[]){b}, n)
    /* movsd xmm2, [rdi + ra_for*SLOT_SIZE] ; limit */
    { unsigned char b[] = {0xF2, 0x0F, 0x10, 0x97};
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)(ra_for * (int)SLOT_SIZE)); }
    /* movsd xmm1, [rdi + (ra_for+1)*SLOT_SIZE] ; step */
    { unsigned char b[] = {0xF2, 0x0F, 0x10, 0x8F};
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)((ra_for + 1) * (int)SLOT_SIZE)); }
    /* movsd xmm0, [rdi + (ra_for+2)*SLOT_SIZE] ; idx */
    { unsigned char b[] = {0xF2, 0x0F, 0x10, 0x87};
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)((ra_for + 2) * (int)SLOT_SIZE)); }

    /* Float register promotion: pin accumulator to xmm5.
       Reuse the same npinned/pinned_slot from int analysis. */
    /* If accumulator found, load into xmm5 before loop */
    int fpinned = (npinned >= 1) ? pinned_slot[0] : -1;
    if (fpinned >= 0) {
      /* movsd xmm5, [rdi + slot*SLOT] */
      unsigned char bb[] = {0xF2, 0x0F, 0x10, 0xAF};
      emit_bytes(&fem, bb, 4);
      emit_u32(&fem, (unsigned int)(fpinned * (int)SLOT_SIZE));
    }

    /* === Loop top === */
    int floop_top = (int)fem.pos;

    /* === Compile loop body (float versions of ops) === */
    for (i = pc + 1; i < loop_end_pc; i++) {
      Instruction inst = code[i];
      OpCode op = GET_OPCODE(inst);
      int a = GETARG_A(inst);
      switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_DIV: case OP_IDIV: {
          int b2 = GETARG_B(inst);
          int c2 = GETARG_C(inst);
          unsigned char opc;
          if (op == OP_ADD) opc = 0x58;
          else if (op == OP_SUB) opc = 0x5C;
          else if (op == OP_MUL) opc = 0x59;
          else opc = 0x5E;  /* DIV/IDIV → divsd */
          /* Fast path: accumulator op loop_var → pure register */
          if (fpinned >= 0 && a == fpinned && b2 == fpinned
              && c2 == ra_for + 2) {
            /* xmm5 op= xmm0 (accumulator += loop var) */
            unsigned char bb[] = {0xF2, 0x0F, opc, 0xE8};
            emit_bytes(&fem, bb, 4);
          }
          else if (fpinned >= 0 && a == fpinned && c2 == fpinned
                   && b2 == ra_for + 2 && op == OP_ADD) {
            /* xmm5 += xmm0 (reversed, addition is commutative) */
            unsigned char bb[] = {0xF2, 0x0F, 0x58, 0xE8};
            emit_bytes(&fem, bb, 4);
          }
          else {
            /* Generic: load from memory, op, store */
            { unsigned char bb[] = {0xF2, 0x0F, 0x10, 0x9F};
              emit_bytes(&fem, bb, 4);
              emit_u32(&fem, (unsigned int)(b2 * (int)SLOT_SIZE)); }
            { unsigned char bb[] = {0xF2, 0x0F, opc, 0x9F};
              emit_bytes(&fem, bb, 4);
              emit_u32(&fem, (unsigned int)(c2 * (int)SLOT_SIZE)); }
            if (fpinned >= 0 && a == fpinned) {
              /* movsd xmm5, xmm3 */
              unsigned char bb[] = {0xF2, 0x0F, 0x10, 0xEB};
              emit_bytes(&fem, bb, 4);
            }
            else {
              unsigned char bb[] = {0xF2, 0x0F, 0x11, 0x9F};
              emit_bytes(&fem, bb, 4);
              emit_u32(&fem, (unsigned int)(a * (int)SLOT_SIZE));
            }
          }
          break;
        }
        case OP_ADDI: {
          int b2 = GETARG_B(inst);
          int sc = GETARG_sC(inst);
          /* load R[B] */
          { unsigned char bb[] = {0xF2, 0x0F, 0x10, 0x9F};
            emit_bytes(&fem, bb, 4);
            emit_u32(&fem, (unsigned int)(b2 * (int)SLOT_SIZE)); }
          /* need to convert int imm to double - use stack temp */
          /* push imm as int, cvtsi2sd */
          /* mov eax, imm; cvtsi2sd xmm4, eax; addsd xmm3, xmm4 */
          { unsigned char bb[] = {0xB8}; emit_bytes(&fem, bb, 1);
            emit_u32(&fem, (unsigned int)sc); }
          { unsigned char bb[] = {0xF2, 0x0F, 0x2A, 0xE0};
            emit_bytes(&fem, bb, 4); } /* cvtsi2sd xmm4, eax */
          { unsigned char bb[] = {0xF2, 0x0F, 0x58, 0xDC};
            emit_bytes(&fem, bb, 4); } /* addsd xmm3, xmm4 */
          { unsigned char bb[] = {0xF2, 0x0F, 0x11, 0x9F};
            emit_bytes(&fem, bb, 4);
            emit_u32(&fem, (unsigned int)(a * (int)SLOT_SIZE)); }
          break;
        }
        case OP_MOVE: {
          int b2 = GETARG_B(inst);
          { unsigned char bb[] = {0xF2, 0x0F, 0x10, 0x9F};
            emit_bytes(&fem, bb, 4);
            emit_u32(&fem, (unsigned int)(b2 * (int)SLOT_SIZE)); }
          { unsigned char bb[] = {0xF2, 0x0F, 0x11, 0x9F};
            emit_bytes(&fem, bb, 4);
            emit_u32(&fem, (unsigned int)(a * (int)SLOT_SIZE)); }
          break;
        }
        case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
        case OP_LOADI: case OP_LOADK: case OP_ADDK:
        case OP_SUBK: case OP_MULK:
          break; /* skip or handle later */
        default:
          break;
      }
    }

    /* === Float loop update: idx += step; if idx <= limit continue === */
    /* addsd xmm0, xmm1 (idx += step) */
    { unsigned char b[] = {0xF2, 0x0F, 0x58, 0xC1};
      emit_bytes(&fem, b, 4); }
    /* store updated idx to R[A+2] */
    { unsigned char b[] = {0xF2, 0x0F, 0x11, 0x87};
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)((ra_for + 2) * (int)SLOT_SIZE)); }
    /* comisd xmm0, xmm2 (compare idx with limit) */
    { unsigned char b[] = {0x66, 0x0F, 0x2F, 0xC2};
      emit_bytes(&fem, b, 4); }
    /* jbe floop_top (jump if idx <= limit, i.e., CF=1 or ZF=1) */
    { int rel = floop_top - ((int)fem.pos + 6);
      unsigned char b[] = {0x0F, 0x86};
      emit_bytes(&fem, b, 2);
      emit_u32(&fem, (unsigned int)rel); }

    /* store final idx */
    { unsigned char b[] = {0xF2, 0x0F, 0x11, 0x87};
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)((ra_for + 2) * (int)SLOT_SIZE)); }
    /* store pinned float accumulator (xmm5) back */
    if (fpinned >= 0) {
      unsigned char b[] = {0xF2, 0x0F, 0x11, 0xAF};  /* movsd [rdi+disp], xmm5 */
      emit_bytes(&fem, b, 4);
      emit_u32(&fem, (unsigned int)(fpinned * (int)SLOT_SIZE));
    }

    jit_emit_epilogue(&fem);
    fcode_size = fem.pos;
  }

  /* Create trace with both int and float paths */
  trace = luaM_new(L, JitTrace);
  trace->code = buf;
  trace->code_size = em.pos;
  trace->fcode = fbuf;
  trace->fcode_size = fcode_size;
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
