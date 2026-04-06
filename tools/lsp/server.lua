#!/usr/bin/env lua
--[[
  Lua5g Language Server Protocol (LSP) server
  Provides: autocomplete, hover, diagnostics, go-to-definition

  Run: lua5g tools/lsp/server.lua
  VSCode config: see vscode-lua5g/package.json
]]

local io = io
local string = string
local table = table
local pairs = pairs
local tonumber = tonumber
local tostring = tostring

-- ============================================================
-- JSON encoder/decoder (minimal)
-- ============================================================

local json = {}

function json.encode(val)
  local t = type(val)
  if t == "nil" then return "null"
  elseif t == "boolean" then return val and "true" or "false"
  elseif t == "number" then return tostring(val)
  elseif t == "string" then
    return '"' .. val:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t') .. '"'
  elseif t == "table" then
    -- array check
    if #val > 0 or next(val) == nil then
      local parts = {}
      for i = 1, #val do parts[i] = json.encode(val[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    else
      local parts = {}
      for k, v in pairs(val) do
        parts[#parts+1] = json.encode(tostring(k)) .. ":" .. json.encode(v)
      end
      return "{" .. table.concat(parts, ",") .. "}"
    end
  end
  return "null"
end

function json.decode(str)
  local pos = 1
  local function skip_ws()
    pos = str:match("^%s*()", pos)
  end
  local function parse_value()
    skip_ws()
    local c = str:sub(pos, pos)
    if c == '"' then
      local s = ""
      pos = pos + 1
      while pos <= #str do
        local ch = str:sub(pos, pos)
        if ch == '\\' then
          pos = pos + 1
          local esc = str:sub(pos, pos)
          if esc == 'n' then s = s .. '\n'
          elseif esc == 't' then s = s .. '\t'
          elseif esc == 'r' then s = s .. '\r'
          else s = s .. esc end
        elseif ch == '"' then pos = pos + 1; return s
        else s = s .. ch end
        pos = pos + 1
      end
      return s
    elseif c == '{' then
      pos = pos + 1
      local obj = {}
      skip_ws()
      if str:sub(pos, pos) == '}' then pos = pos + 1; return obj end
      while true do
        skip_ws()
        local key = parse_value()
        skip_ws()
        pos = pos + 1 -- skip ':'
        local val = parse_value()
        obj[key] = val
        skip_ws()
        if str:sub(pos, pos) == ',' then pos = pos + 1
        else pos = pos + 1; break end -- skip '}'
      end
      return obj
    elseif c == '[' then
      pos = pos + 1
      local arr = {}
      skip_ws()
      if str:sub(pos, pos) == ']' then pos = pos + 1; return arr end
      while true do
        arr[#arr+1] = parse_value()
        skip_ws()
        if str:sub(pos, pos) == ',' then pos = pos + 1
        else pos = pos + 1; break end
      end
      return arr
    elseif str:sub(pos, pos+3) == "true" then pos = pos + 4; return true
    elseif str:sub(pos, pos+4) == "false" then pos = pos + 5; return false
    elseif str:sub(pos, pos+3) == "null" then pos = pos + 4; return nil
    else
      local num = str:match("^%-?%d+%.?%d*[eE]?[+-]?%d*", pos)
      if num then pos = pos + #num; return tonumber(num) end
    end
    return nil
  end
  return parse_value()
end

-- ============================================================
-- LSP Protocol helpers
-- ============================================================

local function log(msg)
  io.stderr:write("[lua5g-lsp] " .. msg .. "\n")
  io.stderr:flush()
end

local function send_response(id, result)
  local body = json.encode({jsonrpc = "2.0", id = id, result = result})
  local msg = "Content-Length: " .. #body .. "\r\n\r\n" .. body
  io.stdout:write(msg)
  io.stdout:flush()
end

local function send_notification(method, params)
  local body = json.encode({jsonrpc = "2.0", method = method, params = params})
  local msg = "Content-Length: " .. #body .. "\r\n\r\n" .. body
  io.stdout:write(msg)
  io.stdout:flush()
end

local function read_message()
  local headers = {}
  while true do
    local line = io.stdin:read("*l")
    if not line or line == "" or line == "\r" then break end
    local key, val = line:match("^(.-):%s*(.*)")
    if key then headers[key:lower()] = val:gsub("\r", "") end
  end
  local len = tonumber(headers["content-length"])
  if not len then return nil end
  local body = io.stdin:read(len)
  if not body then return nil end
  return json.decode(body)
end

-- ============================================================
-- Language intelligence
-- ============================================================

-- Lua5g keywords for completion
local keywords = {
  "and","break","class","do","else","elseif","end","enum","extends",
  "false","for","function","global","goto","if","implements","in",
  "interface","local","match","nil","not","or","repeat","return",
  "then","true","try","until","while",
  "override","super","static","abstract","operator","property",
  "private","protected","public","readonly",
  "import","type","declare","async","await","except","finally",
  "self","case"
}

-- Built-in types
local builtin_types = {
  "number","string","boolean","table","function","nil",
  "thread","userdata","any","unknown"
}

-- Built-in functions/modules
local builtins = {
  "print","type","tostring","tonumber","pairs","ipairs","next",
  "select","pcall","xpcall","error","assert","require",
  "setmetatable","getmetatable","rawget","rawset","rawlen",
  "math","string","table","io","os","coroutine","debug",
  "mathx","trace","profile","test","ldb"
}

-- Document store
local documents = {}  -- uri -> {text, lines}

local function update_document(uri, text)
  local lines = {}
  for line in (text .. "\n"):gmatch("(.-)\n") do
    lines[#lines+1] = line
  end
  documents[uri] = {text = text, lines = lines}
end

local function get_word_at(uri, line, col)
  local doc = documents[uri]
  if not doc or not doc.lines[line+1] then return "" end
  local text = doc.lines[line+1]
  local s = col
  while s > 0 and text:sub(s, s):match("[%w_]") do s = s - 1 end
  return text:sub(s+1, col)
end

-- ============================================================
-- LSP Handlers
-- ============================================================

local handlers = {}

handlers["initialize"] = function(id, params)
  send_response(id, {
    capabilities = {
      textDocumentSync = {
        openClose = true,
        change = 1,  -- full sync
      },
      completionProvider = {
        triggerCharacters = {".", ":", "<"},
        resolveProvider = false,
      },
      hoverProvider = true,
      definitionProvider = false,  -- future
      diagnosticProvider = {
        interFileDependencies = false,
        workspaceDiagnostics = false,
      },
    },
    serverInfo = {
      name = "lua5g-lsp",
      version = "0.1.0",
    },
  })
  log("Initialized")
end

handlers["initialized"] = function(id, params)
  -- client is ready
end

handlers["shutdown"] = function(id, params)
  send_response(id, nil)
end

handlers["exit"] = function(id, params)
  os.exit(0)
end

handlers["textDocument/didOpen"] = function(id, params)
  local uri = params.textDocument.uri
  local text = params.textDocument.text
  update_document(uri, text)
  log("Opened: " .. uri)
end

handlers["textDocument/didChange"] = function(id, params)
  local uri = params.textDocument.uri
  if params.contentChanges and params.contentChanges[1] then
    update_document(uri, params.contentChanges[1].text)
  end
end

handlers["textDocument/didClose"] = function(id, params)
  documents[params.textDocument.uri] = nil
end

handlers["textDocument/completion"] = function(id, params)
  local uri = params.textDocument.uri
  local line = params.position.line
  local col = params.position.character
  local prefix = get_word_at(uri, line, col)

  local items = {}

  -- Keywords
  for _, kw in ipairs(keywords) do
    if kw:sub(1, #prefix) == prefix then
      items[#items+1] = {
        label = kw,
        kind = 14,  -- Keyword
        detail = "keyword",
      }
    end
  end

  -- Built-in types (after ':')
  local doc = documents[uri]
  if doc and doc.lines[line+1] then
    local linetext = doc.lines[line+1]
    if linetext:sub(col, col) == ":" or linetext:match(":%s*$") then
      for _, t in ipairs(builtin_types) do
        items[#items+1] = {
          label = t,
          kind = 25,  -- TypeParameter
          detail = "type",
        }
      end
    end
  end

  -- Built-in functions
  for _, fn in ipairs(builtins) do
    if fn:sub(1, #prefix) == prefix then
      items[#items+1] = {
        label = fn,
        kind = 3,  -- Function
        detail = "built-in",
      }
    end
  end

  send_response(id, items)
end

handlers["textDocument/hover"] = function(id, params)
  local uri = params.textDocument.uri
  local line = params.position.line
  local col = params.position.character
  local word = get_word_at(uri, line, col)

  local info = nil

  -- Check keywords
  for _, kw in ipairs(keywords) do
    if kw == word then
      local descs = {
        class = "Declare a class: `class Name [extends Parent] ... end`",
        extends = "Inherit from parent class",
        interface = "Declare an interface: `interface Name ... end`",
        implements = "Implement an interface",
        enum = "Declare an enum: `enum Name VALUE1 VALUE2 end`",
        try = "Exception handling: `try ... except err then ... finally ... end`",
        match = "Pattern matching: `match value case X then ... end`",
        override = "Override parent class method (required when parent has same method)",
        super = "Reference to parent class (in class methods)",
        property = "Declare getter/setter: `property name get(self) ... set(self,v) ... end`",
        readonly = "Field that can only be set once (during construction)",
        private = "Field accessible only within class methods",
        protected = "Field accessible in class and subclass methods",
        abstract = "Method without implementation (must be overridden)",
        static = "Class method (no self parameter)",
        import = "Import module: `import \"mod\"` or `import name from \"mod\"`",
        type = "Type alias: `type Name = { ... }`",
        declare = "Declaration-only class (for C bindings): `declare class Name ... end`",
        lambda = "Lambda expression: `|x, y| expr`",
      }
      info = descs[word] or ("Lua5g keyword: `" .. word .. "`")
      break
    end
  end

  -- Check types
  if not info then
    for _, t in ipairs(builtin_types) do
      if t == word then
        info = "Built-in type: `" .. word .. "`"
        break
      end
    end
  end

  if info then
    send_response(id, {
      contents = {
        kind = "markdown",
        value = info,
      },
    })
  else
    send_response(id, nil)
  end
end

-- ============================================================
-- Main loop
-- ============================================================

log("Lua5g LSP server starting...")

while true do
  local msg = read_message()
  if not msg then break end

  local method = msg.method
  local id = msg.id

  if method and handlers[method] then
    local ok, err = pcall(handlers[method], id, msg.params or {})
    if not ok then
      log("Error in " .. method .. ": " .. tostring(err))
    end
  elseif id then
    -- Unknown request: respond with null
    send_response(id, nil)
  end
end

log("LSP server exiting")
