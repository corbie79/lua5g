// Lua5g Debug Adapter - bridges VSCode DAP to lua5g debugger.lua
const { spawn } = require('child_process');
const path = require('path');

// The debugger is a Lua script that speaks DAP over stdin/stdout
const luaPath = process.env.LUA5G_PATH || 'lua5g';
const debugScript = path.join(__dirname, '..', 'tools', 'lsp', 'debugger.lua');

const child = spawn(luaPath, [debugScript], {
  stdio: ['pipe', 'pipe', 'inherit']
});

// Forward stdin -> child.stdin (VSCode -> debugger)
process.stdin.pipe(child.stdin);

// Forward child.stdout -> stdout (debugger -> VSCode)
child.stdout.pipe(process.stdout);

child.on('exit', (code) => {
  process.exit(code || 0);
});

process.on('SIGTERM', () => {
  child.kill();
});
