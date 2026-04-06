#!/usr/bin/env lua
--[[
  docgen.lua - Generate markdown API documentation from .d.lua files

  Usage: lua docgen.lua input.d.lua > output.md
         lua docgen.lua src/*.d.lua > api.md
]]

local function parse_file(filename)
  local f = io.open(filename, "r")
  if not f then
    io.stderr:write("Cannot open: " .. filename .. "\n")
    return {}
  end
  local content = f:read("*a")
  f:close()

  local entries = {}

  -- Parse declare class
  for classname, parent, body in content:gmatch(
      "declare%s+class%s+(%w+)%s*(.-)%s*\n(.-)\nend") do
    local entry = {
      type = "class",
      name = classname,
      parent = parent:match("parent%s+(%w+)") or nil,
      methods = {},
      fields = {},
    }
    -- Parse methods
    for mname, params, ret in body:gmatch(
        "function%s+(%w+)%s*%((.-)%)%s*:?%s*(%w*)") do
      table.insert(entry.methods, {
        name = mname,
        params = params,
        returns = ret ~= "" and ret or nil,
      })
    end
    -- Parse fields
    for laccess, fname, ftype in body:gmatch(
        "(%w*)%s*(%w+)%s*:%s*(%w+)") do
      if laccess == "" then access = "public" end
      if fname ~= "function" then
        table.insert(entry.fields, {
          name = fname,
          type = ftype,
          access = laccess,
        })
      end
    end
    table.insert(entries, entry)
  end

  -- Parse interface
  for iname, body in content:gmatch(
      "interface%s+(%w+)%s*\n(.-)\nend") do
    local entry = {
      type = "interface",
      name = iname,
      methods = {},
    }
    for mname, params, ret in body:gmatch(
        "function%s+(%w+)%s*%((.-)%)%s*:?%s*(%w*)") do
      table.insert(entry.methods, {
        name = mname,
        params = params,
        returns = ret ~= "" and ret or nil,
      })
    end
    table.insert(entries, entry)
  end

  -- Parse enum
  for ename, body in content:gmatch("enum%s+(%w+)%s+(.-)\nend") do
    local values = {}
    for v in body:gmatch("(%w+)") do
      table.insert(values, v)
    end
    table.insert(entries, {
      type = "enum",
      name = ename,
      values = values,
    })
  end

  return entries
end

local function generate_md(entries, filename)
  local out = {}
  table.insert(out, "# API Reference")
  table.insert(out, "")
  table.insert(out, "*Generated from: " .. (filename or "unknown") .. "*")
  table.insert(out, "")

  for _, e in ipairs(entries) do
    if e.type == "class" then
      table.insert(out, "## Class: `" .. e.name .. "`")
      if e.parent then
        table.insert(out, "")
        table.insert(out, "Extends: `" .. e.parent .. "`")
      end
      table.insert(out, "")

      if #e.fields > 0 then
        table.insert(out, "### Fields")
        table.insert(out, "")
        table.insert(out, "| Name | Type | Access |")
        table.insert(out, "|------|------|--------|")
        for _, f in ipairs(e.fields) do
          table.insert(out, "| `" .. f.name .. "` | `" .. f.type .. "` | " .. f.access .. " |")
        end
        table.insert(out, "")
      end

      if #e.methods > 0 then
        table.insert(out, "### Methods")
        table.insert(out, "")
        for _, m in ipairs(e.methods) do
          local sig = "`" .. e.name .. ":" .. m.name .. "(" .. m.params .. ")`"
          if m.returns then sig = sig .. " → `" .. m.returns .. "`" end
          table.insert(out, "- " .. sig)
        end
        table.insert(out, "")
      end

    elseif e.type == "interface" then
      table.insert(out, "## Interface: `" .. e.name .. "`")
      table.insert(out, "")
      if #e.methods > 0 then
        table.insert(out, "### Required Methods")
        table.insert(out, "")
        for _, m in ipairs(e.methods) do
          local sig = "`" .. m.name .. "(" .. m.params .. ")`"
          if m.returns then sig = sig .. " → `" .. m.returns .. "`" end
          table.insert(out, "- " .. sig)
        end
        table.insert(out, "")
      end

    elseif e.type == "enum" then
      table.insert(out, "## Enum: `" .. e.name .. "`")
      table.insert(out, "")
      table.insert(out, "Values: " .. table.concat(e.values, ", "))
      table.insert(out, "")
    end
  end

  return table.concat(out, "\n")
end

-- Main
local files = {...}
if #files == 0 then
  io.stderr:write("Usage: lua docgen.lua file1.d.lua [file2.d.lua ...]\n")
  os.exit(1)
end

local all = {}
for _, fn in ipairs(files) do
  local entries = parse_file(fn)
  for _, e in ipairs(entries) do
    table.insert(all, e)
  end
end
print(generate_md(all, table.concat(files, ", ")))
