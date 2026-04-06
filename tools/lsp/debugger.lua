#!/usr/bin/env lua
--[[
  Lua5g Debug Adapter Protocol (DAP) server
  Provides: breakpoints, step, variable inspection

  Communicates via stdin/stdout JSON-RPC (DAP protocol).
  Launched by VSCode when debugging a .lua file.
]]

local io = io
local os = os
local string = string
local table = table
local debug = debug
local pairs = pairs
local tostring = tostring
local tonumber = tonumber
local type = type

-- Reuse json from LSP
local json = dofile(arg[0]:match("(.*/)")  .. "json.lua") or {}

-- Minimal JSON (inline if json.lua not found)
if not json.encode then
  json = {}
  function json.encode(v)
    local t = type(v)
    if t == "nil" then return "null"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then return tostring(v)
    elseif t == "string" then return '"' .. v:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n') .. '"'
    elseif t == "table" then
      if #v > 0 then
        local p = {}; for i=1,#v do p[i]=json.encode(v[i]) end
        return "[" .. table.concat(p,",") .. "]"
      else
        local p = {}; for k,val in pairs(v) do p[#p+1]=json.encode(tostring(k))..":"..json.encode(val) end
        return "{" .. table.concat(p,",") .. "}"
      end
    end
    return "null"
  end
  function json.decode(s)
    -- simplified: use load for trusted input
    s = s:gsub('"([^"]-)":', '["%1"]='):gsub('%[%]','{}')
    local f = load("return " .. s)
    return f and f() or nil
  end
end

-- ============================================================
-- DAP Protocol
-- ============================================================

local seq = 1

local function log(msg)
  io.stderr:write("[lua5g-dap] " .. msg .. "\n")
  io.stderr:flush()
end

local function send_event(event, body)
  local msg = json.encode({
    seq = seq, type = "event", event = event, body = body or {}
  })
  seq = seq + 1
  io.stdout:write("Content-Length: " .. #msg .. "\r\n\r\n" .. msg)
  io.stdout:flush()
end

local function send_response(request, success, body)
  local msg = json.encode({
    seq = seq, type = "response",
    request_seq = request.seq,
    command = request.command,
    success = success,
    body = body or {}
  })
  seq = seq + 1
  io.stdout:write("Content-Length: " .. #msg .. "\r\n\r\n" .. msg)
  io.stdout:flush()
end

local function read_message()
  local headers = {}
  while true do
    local line = io.stdin:read("*l")
    if not line or line == "" or line == "\r" then break end
    local k, v = line:match("^(.-):%s*(.*)")
    if k then headers[k:lower()] = v:gsub("\r","") end
  end
  local len = tonumber(headers["content-length"])
  if not len then return nil end
  local body = io.stdin:read(len)
  return body and json.decode(body) or nil
end

-- ============================================================
-- Debug State
-- ============================================================

local breakpoints = {}  -- file -> {line -> true}
local stopped = false
local stepping = false  -- step mode
local step_depth = 0
local target_file = nil
local debug_hook_installed = false

-- Collect local variables at a given stack level
local function get_locals(level)
  local vars = {}
  local i = 1
  while true do
    local name, value = debug.getlocal(level + 1, i)
    if not name then break end
    if name:sub(1,1) ~= '(' then  -- skip internal vars
      local vtype = type(value)
      local display
      if vtype == "string" then
        display = '"' .. (value:sub(1,50)) .. '"'
      elseif vtype == "table" then
        local count = 0
        for _ in pairs(value) do count = count + 1 end
        display = "table (" .. count .. " fields)"
      elseif vtype == "function" then
        local info = debug.getinfo(value, "S")
        display = "function@" .. (info.short_src or "?") .. ":" .. (info.linedefined or 0)
      else
        display = tostring(value)
      end
      vars[#vars+1] = {
        name = name,
        value = display,
        type = vtype,
        variablesReference = (vtype == "table") and i or 0,
      }
    end
    i = i + 1
  end
  return vars
end

-- Debug hook function
local function debug_hook(event, line)
  if event ~= "line" then return end

  local info = debug.getinfo(2, "S")
  local src = info.source
  if src:sub(1,1) == "@" then src = src:sub(2) end

  -- Check breakpoint
  local hit = false
  if breakpoints[src] and breakpoints[src][line] then
    hit = true
  end

  -- Check stepping
  if stepping then
    hit = true
  end

  if hit then
    stopped = true
    send_event("stopped", {
      reason = stepping and "step" or "breakpoint",
      threadId = 1,
      allThreadsStopped = true,
    })

    -- Wait for commands while stopped
    while stopped do
      local msg = read_message()
      if not msg then os.exit(0) end
      handle_debug_command(msg)
    end
  end
end

-- ============================================================
-- DAP Command Handlers
-- ============================================================

local dap_handlers = {}

dap_handlers["initialize"] = function(req)
  send_response(req, true, {
    supportsConfigurationDoneRequest = true,
    supportsFunctionBreakpoints = false,
    supportsConditionalBreakpoints = false,
    supportsEvaluateForHovers = true,
  })
  send_event("initialized")
end

dap_handlers["configurationDone"] = function(req)
  send_response(req, true)
end

dap_handlers["launch"] = function(req)
  target_file = req.arguments and req.arguments.program
  send_response(req, true)

  if target_file then
    -- Install debug hook and run file
    debug.sethook(debug_hook, "l")
    debug_hook_installed = true
    local ok, err = pcall(dofile, target_file)
    debug.sethook()
    if not ok then
      send_event("output", {
        category = "stderr",
        output = "Error: " .. tostring(err) .. "\n"
      })
    end
    send_event("terminated")
  end
end

dap_handlers["setBreakpoints"] = function(req)
  local source = req.arguments.source
  local path = source.path or ""
  breakpoints[path] = {}
  local result_bps = {}
  if req.arguments.breakpoints then
    for _, bp in ipairs(req.arguments.breakpoints) do
      breakpoints[path][bp.line] = true
      result_bps[#result_bps+1] = {verified = true, line = bp.line}
    end
  end
  send_response(req, true, {breakpoints = result_bps})
end

dap_handlers["threads"] = function(req)
  send_response(req, true, {
    threads = {{id = 1, name = "main"}}
  })
end

dap_handlers["stackTrace"] = function(req)
  local frames = {}
  local level = 3  -- skip debug_hook + hook caller
  while true do
    local info = debug.getinfo(level, "Sln")
    if not info then break end
    local src = info.source or "?"
    if src:sub(1,1) == "@" then src = src:sub(2) end
    frames[#frames+1] = {
      id = level,
      name = info.name or "(anonymous)",
      source = {name = src, path = src},
      line = info.currentline or 0,
      column = 0,
    }
    level = level + 1
    if level > 20 then break end
  end
  send_response(req, true, {
    stackFrames = frames,
    totalFrames = #frames,
  })
end

dap_handlers["scopes"] = function(req)
  send_response(req, true, {
    scopes = {{
      name = "Locals",
      variablesReference = 1,
      expensive = false,
    }}
  })
end

dap_handlers["variables"] = function(req)
  local vars = get_locals(4)  -- adjust stack level
  send_response(req, true, {variables = vars})
end

dap_handlers["continue"] = function(req)
  stepping = false
  stopped = false
  send_response(req, true, {allThreadsContinued = true})
end

dap_handlers["next"] = function(req)  -- step over
  stepping = true
  stopped = false
  send_response(req, true)
end

dap_handlers["stepIn"] = function(req)
  stepping = true
  stopped = false
  send_response(req, true)
end

dap_handlers["stepOut"] = function(req)
  stepping = true
  stopped = false
  send_response(req, true)
end

dap_handlers["evaluate"] = function(req)
  local expr = req.arguments and req.arguments.expression
  if expr then
    local fn, err = load("return " .. expr)
    if fn then
      local ok, result = pcall(fn)
      if ok then
        send_response(req, true, {result = tostring(result), variablesReference = 0})
      else
        send_response(req, true, {result = "Error: " .. tostring(result), variablesReference = 0})
      end
    else
      send_response(req, true, {result = "Parse error: " .. tostring(err), variablesReference = 0})
    end
  else
    send_response(req, true, {result = "", variablesReference = 0})
  end
end

dap_handlers["disconnect"] = function(req)
  send_response(req, true)
  os.exit(0)
end

function handle_debug_command(msg)
  local cmd = msg.command
  if cmd and dap_handlers[cmd] then
    local ok, err = pcall(dap_handlers[cmd], msg)
    if not ok then log("DAP error: " .. tostring(err)) end
  elseif msg.seq then
    send_response(msg, false)
  end
end

-- ============================================================
-- Main
-- ============================================================

log("Lua5g DAP debugger starting...")

while true do
  local msg = read_message()
  if not msg then break end
  handle_debug_command(msg)
end
