--[[============================================================================
  tree_smoke.lua — exercise the WIRESHARK-SIDE half of libtracer.lua.

  `run_tests.py` drives the pure decoder through `--decode-json`, which is most of
  the file but deliberately NOT the registration block: ProtoFields, the expert
  infos and `add_node`'s tree rendering only run when a `Proto` global exists, and
  CI has no Wireshark. That half was therefore covered by `luac5.4 -p` alone — a
  syntax check, which is blind to a field declared `uint16` and added from a
  four-byte slice, to a wrong argument order, or to a range that runs off the end
  of the buffer.

  So this file stubs the Wireshark API just far enough to load the dissector with
  `Proto` present, feed it a frame, and assert on what it put on the tree. It is
  a smoke test, not a rendering fixture: it checks that the tree code RUNS and
  that the values it adds are the decoded ones.

  Run: lua tools/wireshark/tests/tree_smoke.lua [path/to/libtracer.lua]
  Exit 0 on success; prints one [PASS]/[FAIL] line per case and fails loudly.
============================================================================]]--

local LUA_FILE = arg and arg[1]
if not LUA_FILE then
  local here = debug.getinfo(1, "S").source:sub(2)
  LUA_FILE = here:gsub("tests/tree_smoke%.lua$", "libtracer.lua")
end

--============================================================================
-- The stub Wireshark API. Only what libtracer.lua actually touches.
--============================================================================

--[[ @brief A byte range, standing in for Wireshark's TvbRange. ]]--
local function make_range(bytes, off, len)
  if off < 0 or len < 0 or off + len > #bytes then
    error(string.format("tvb range out of bounds: off=%d len=%d buffer=%d", off, len, #bytes))
  end
  local r = { off = off, len = len }
  --[[ @brief The range's bytes, little-endian, as an unsigned integer. ]]--
  function r:uint_le()
    local v = 0
    for i = self.len - 1, 0, -1 do v = v * 256 + string.byte(bytes, self.off + 1 + i) end
    return v
  end
  return r
end

--[[ @brief A Tvb over `bytes`: callable for ranges, with :len() and :raw(). ]]--
local function make_tvb(bytes)
  local t = setmetatable({}, { __call = function(_, off, len) return make_range(bytes, off, len) end })
  function t:len() return #bytes end
  function t:raw() return bytes end
  return t
end

-- Everything the dissector adds lands here, in order.
local added = {}

local item = {}
item.__index = item
function item:add(field, range, value)
  added[#added + 1] = { abbr = field.abbr, range = range, value = value, le = false }
  return setmetatable({}, item)
end
function item:add_le(field, range)
  added[#added + 1] = { abbr = field.abbr, range = range, value = range:uint_le(), le = true }
  return setmetatable({}, item)
end
function item:add_proto_expert_info(exp, msg)
  added[#added + 1] = { expert = exp.abbr, msg = msg }
  return setmetatable({}, item)
end
function item:append_text(_) return self end
function item:set_generated() return self end

local function new_field(abbr) return { abbr = abbr } end

-- The Proto the dissector builds, captured so its `dissector` closure is reachable.
local captured_proto = nil
_G.Proto = function(_, _)
  local p = { prefs = {} }
  function p:register_heuristic(_, _) end
  captured_proto = p
  return p
end
_G.ProtoField = setmetatable({}, {
  __index = function(_, _) return function(abbr) return new_field(abbr) end end,
})
_G.ProtoExpert = { new = function(abbr) return { abbr = abbr } end }
_G.base = setmetatable({}, { __index = function() return 0 end })
_G.expert = { group = setmetatable({}, { __index = function() return 0 end }),
              severity = setmetatable({}, { __index = function() return 0 end }) }
_G.Pref = { uint = function(_, v) return v end, bool = function(_, v) return v end }
_G.DissectorTable = { get = function() error("no dissector tables in the stub") end }

dofile(LUA_FILE)
local proto_dissector = captured_proto and captured_proto.dissector

--============================================================================
-- Cases
--============================================================================
local function hex_to_bytes(hex)
  local t = {}
  for pair in hex:gmatch("%x%x") do t[#t + 1] = string.char(tonumber(pair, 16)) end
  return table.concat(t)
end

--[[ @brief Dissect `hex` through the stubbed tree and return the added items. ]]--
local function dissect(hex)
  added = {}
  local bytes = hex_to_bytes(hex)
  local tvb = make_tvb(bytes)
  local pinfo = { cols = { protocol = "", info = { set = function() end } } }
  local tree = setmetatable({}, item)
  proto_dissector(tvb, pinfo, tree)
  return added
end

--[[ @brief The first added item whose field abbreviation is `abbr`, or nil. ]]--
local function field(items, abbr)
  for _, it in ipairs(items) do if it.abbr == abbr then return it end end
  return nil
end

--[[ @brief Every added expert-info abbreviation, in order. ]]--
local function experts(items)
  local out = {}
  for _, it in ipairs(items) do if it.expert then out[#out + 1] = it.expert end end
  return out
end

local failures = 0
local function check(ok, what)
  print((ok and "  [PASS] " or "  [FAIL] ") .. what)
  if not ok then failures = failures + 1 end
end

print("libtracer.lua tree smoke (stubbed Wireshark API)")

if not proto_dissector then
  print("  [FAIL] the registration block installed no dissector")
  os.exit(1)
end

-- path-label/label-roundtrip: one label, index 1 at generation 2.
do
  local items = dissect("0600070000160401000200")
  local v = field(items, "libtracer.path.label")
  local ix = field(items, "libtracer.path.label.index")
  local gn = field(items, "libtracer.path.label.generation")
  check(v ~= nil and v.value == 0x00020001, "label value is the verbatim LE u32")
  check(ix ~= nil and ix.value == 1, "label index reads 1 from its own two bytes")
  check(gn ~= nil and gn.value == 2, "label generation reads 2 from its own two bytes")
  check(ix ~= nil and ix.range.len == 2 and gn ~= nil and gn.range.len == 2,
        "each half is added over TWO bytes — a uint16 field cannot take four")
  check(ix ~= nil and gn ~= nil and gn.range.off == ix.range.off + 2,
        "the generation sits immediately after the index (16/16 split, LE)")
  check(#experts(items) == 0, "a well-formed label raises no expert info")
end

-- path-label/label-wrong-length: kind 0x16, declared length 3.
do
  local items = dissect("06000D000673656E736F72001603010002")
  check(field(items, "libtracer.path.label") == nil,
        "a wrong-length label adds NO label field — the bytes are not read as one")
  check(#experts(items) == 1 and experts(items)[1] == "libtracer.path.label.invalid",
        "…and raises the malformed-path-label expert, not the invalid-FRAME one")
end

-- Generation 0: displayed as the label it claims to be, flagged.
do
  local items = dissect("0600070000160401000000")
  local gn = field(items, "libtracer.path.label.generation")
  check(gn ~= nil and gn.value == 0, "a generation-0 label is still added as a label")
  check(#experts(items) == 1 and experts(items)[1] == "libtracer.path.label.invalid",
        "…and is flagged, because it never legitimately reaches the wire")
end

-- A foreign escape kind is not a label, whatever its payload length.
do
  local items = dissect("06000E000673656E736F72001704DEADBEEF")
  check(field(items, "libtracer.path.label") == nil,
        "a four-byte escape at kind 0x17 adds no label field")
  check(#experts(items) == 0, "…and is not an error either — a relaying hop steps over it")
end

-- Two labels in one address, both rendered (label-mixed).
do
  local items = dissect("06001A000673656E736F72001604010002000474656D700016040201FFFF")
  local n = 0
  for _, it in ipairs(items) do if it.abbr == "libtracer.path.label" then n = n + 1 end end
  check(n == 2, "both labels of a mixed address reach the tree")
  check(field(items, "libtracer.path") ~= nil, "the rendered address is still added")
end

-- A whole FWD frame: the tree walk must reach the PATH children's labels.
do
  local items = dissect(
    "0F403100010001000306000900087265706C792D657006001300001604000001000673656E736F720474656D7001000400D2040000")
  check(field(items, "libtracer.path.label") ~= nil,
        "a label nested inside FWD > PATH is rendered by the recursive walk")
  check(field(items, "libtracer.fwd.dst") ~= nil, "…and the FWD fields still render")
end

print(failures == 0 and "\nall tree-smoke checks passed"
                    or ("\n" .. failures .. " tree-smoke check(s) FAILED"))
os.exit(failures == 0 and 0 or 1)
