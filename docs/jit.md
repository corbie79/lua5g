# JIT Compiler

Lua5g includes a lightweight method JIT that compiles hot numeric for-loops to native machine code.

## How It Works

1. **Detection**: `OP_FORPREP` counts loop entries via `Proto.hotcount`
2. **Threshold**: After 100 entries, `luaJ_compile()` is called
3. **Scan**: Loop body is scanned for supported opcodes
4. **Compile**: Native code is emitted via architecture-specific backend
5. **Execute**: Subsequent calls run native code directly
6. **Fallback**: If compilation fails, interpreter continues (safe)

## Supported Opcodes

| Opcode | Description |
|--------|-------------|
| ADD/SUB/MUL/DIV | Arithmetic (int + float) |
| ADDI/ADDK/SUBK/MULK/DIVK | Constant arithmetic |
| LOADK/LOADI/LOADF | Constant loading |
| MOVE | Register copy |
| MMBIN/MMBINI/MMBINK | Metamethod markers (skipped) |

Loops containing unsupported opcodes (CALL, GETFIELD, SETTABLE, etc.) fall back to the interpreter.

## Register Promotion

The JIT identifies accumulator variables (read + written each iteration) and pins them to CPU registers:

```lua
-- 'sum' is pinned to r14 (x86-64) / r7 (ARM) / edx (x86-32)
local sum = 0
for i = 1, n do
  sum = sum + i     -- pure register operation: add r14, r9
end
```

Before (memory): load + add + store = 3 memory ops per iteration
After (pinned): add reg, reg = 0 memory ops per iteration

## Architecture Backends

### x86-64
- Registers: rdi=base, rbx=count, r8=step, r9=idx, r14=pinned
- Float: SSE2 (xmm0=idx, xmm1=step, xmm2=limit, xmm5=pinned)
- Calling: SysV ABI, `int JitFunc(StackValue *base)`

### x86-32
- Registers: edi=base, ebx=count, esi=step, ecx=idx, edx=pinned
- Float: SSE2 (same encoding as x86-64, no REX prefix)
- Calling: cdecl, base from `[ebp+8]`

### ARMv7
- Registers: r11=base, r4=count, r5=step, r6=idx, r7=pinned
- Float: VFP (d0=idx, d1=step, d2=limit, d5=pinned)
- Instructions: ARM mode (32-bit fixed-width)

### ARM64
- Registers: x28=base, x19=count, x20=step, x21=idx, x22=pinned
- Float: NEON/FP (d0=idx, d1=step, d2=limit, d5=pinned)
- Status: Emitter complete, runtime needs hardware testing

## Benchmark

```
50M integer sum (for i = 1, 50000000 do sum = sum + i end):

  LuaJIT (JIT ON):    0.035 sec  1412 Mops/s
  Lua5g JIT (x64):    0.037 sec  1340 Mops/s  ← 95% of LuaJIT
  LuaJIT (JIT OFF):   0.203 sec   246 Mops/s
  Lua5g interpreter:  0.229 sec   219 Mops/s

  JIT speedup: 6.2x (integer), 6.6x (float)
```

## API

```lua
__jit_compile(func)  -- manually compile function's for-loops
__jit_status()       -- returns: "x86-64", "x86", "arm", "arm64", or "none"
```

## Cache Coherency

On ARM architectures, `__builtin___clear_cache()` is called after code generation to flush the instruction cache.
