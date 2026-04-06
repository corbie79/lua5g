# JIT Compiler

A lightweight method JIT that compiles hot numeric for-loops to native machine code.

---

## How It Works

1. `OP_FORPREP` counts loop entries via `Proto.hotcount`
2. After **100 entries**, `luaJ_compile()` triggers
3. Loop body is scanned for supported opcodes
4. Native code emitted via architecture-specific backend
5. Subsequent calls run native code directly
6. If compilation fails → interpreter continues (safe)

---

## Supported Opcodes

**Arithmetic** — `ADD` `SUB` `MUL` `DIV` (int + float)

**Constant ops** — `ADDI` `ADDK` `SUBK` `MULK` `DIVK` `LOADK` `LOADI` `LOADF`

**Other** — `MOVE`, `MMBIN` / `MMBINI` / `MMBINK` (skipped)

Loops with unsupported opcodes (CALL, GETFIELD, etc.) fall back to the interpreter.

---

## Register Promotion

Accumulator variables are pinned to CPU registers:

```lua
local sum = 0
for i = 1, n do
  sum = sum + i  -- compiles to: add r14, r9 (pure register, 0 memory ops)
end
```

---

## Architectures

### x86-64 (Production)

- `rdi`=base, `rbx`=count, `r8`=step, `r9`=idx, `r14`=pinned
- Float: SSE2 — `xmm0`=idx, `xmm1`=step, `xmm2`=limit, `xmm5`=pinned

### x86-32 (Tested)

- `edi`=base, `ebx`=count, `esi`=step, `ecx`=idx, `edx`=pinned
- Float: SSE2 (same encoding, no REX)

### ARMv7 (QEMU Verified)

- `r11`=base, `r4`=count, `r5`=step, `r6`=idx, `r7`=pinned
- Float: VFP — `d0`=idx, `d1`=step, `d2`=limit, `d5`=pinned

### ARM64 (Emitter Complete)

- `x28`=base, `x19`=count, `x20`=step, `x21`=idx, `x22`=pinned
- Float: NEON — `d0`=idx, `d1`=step, `d2`=limit, `d5`=pinned
- Status: Needs real hardware testing

---

## Benchmark

**50M integer sum** (`for i = 1, 50000000 do sum = sum + i end`):

- **LuaJIT (JIT ON)** — `0.035s` — `1,412 Mops/s`
- **Dala JIT (x64)** — `0.037s` — `1,340 Mops/s` — **95% of LuaJIT**
- **LuaJIT (JIT OFF)** — `0.203s` — `246 Mops/s`
- **Dala interpreter** — `0.229s` — `219 Mops/s`

**Speedup**: 6.2x (integer), 6.6x (float)

---

## API

```lua
__jit_compile(func)  -- manually compile function's for-loops
__jit_status()       -- "x86-64" / "x86" / "arm" / "arm64" / "none"
```

---

## Cache Coherency

ARM/ARM64: `__builtin___clear_cache()` called after code generation.
