--[[============================================================================
  libtracer.lua — Wireshark dissector for the libtracer wire protocol (v1 DRAFT)

  libtracer is a spec-first TLV protocol: every frame is
      header (4/6 bytes) + payload + optional trailer (wire-time TS + CRC).
  This dissector walks that structure, decodes the `opt` bitfield, reconstructs
  PATH addresses to their `/a/b/c` string form, special-cases the FWD/FIELD
  remote-operation frames, and *verifies* the trailer CRC (CRC-32C / CRC-16-CCITT),
  flagging mismatches as expert-info — the headline debugging win.

  Wire format is normative-by-incorporation from:
    docs/reference/01-data-format.md   (frame layout, opt bits, trailer, CRC)
    docs/reference/05-protocol-tlvs.md (the 0x01..0x1F core type registry)
    docs/spec/rfcs/0004-...            (FWD/FIELD op + selector layout)

  DUAL-MODE. The file is loadable two ways:
    * Inside Wireshark/tshark — the `Proto` global exists, so the registration
      block at the bottom installs the dissector, ProtoFields and heuristics.
    * Standalone Lua (CI / unit tests) — no `Proto` global; the file just
      returns the module table `M` exposing the pure decoder `M.parse()` and the
      `M.decode_json()` helper. Run `lua libtracer.lua --decode-json <hexstring>`
      to emit a single JSON line for a frame (used by tools/wireshark/tests).

  PORTABILITY. Wireshark builds bundle assorted Lua versions (5.1 bit32, 5.2,
  5.3/5.4 native operators). Bitwise ops go through a feature-detected shim so
  the file *parses and runs* on every one of them — native operator syntax lives
  only inside a load()-string, never in the file body.

  Status: the libtracer wire format is DRAFT (docs/spec/v1.md line 3). Keep this
  dissector in lockstep with the reference docs; the conformance vectors under
  tests/conformance/vectors/v1/ are its regression corpus.
============================================================================]]--

local M = {}
M.VERSION = "0.1.0"

--============================================================================
-- Portable bitwise shim: native (5.3/5.4) -> bit32 (5.2) -> pure arithmetic.
--============================================================================
local bitops = {}
do
  -- Native operators would be a *syntax* error on 5.1/5.2, so they are compiled
  -- through a string loader; a failed compile just falls through to the next
  -- backend. `loadstring` is the 5.1/LuaJIT spelling; `load(string)` is 5.2+.
  local strload = rawget(_G, "loadstring") or load
  local native = strload([[
    return {
      band   = function(a, b) return a & b end,
      bor    = function(a, b) return a | b end,
      bxor   = function(a, b) return a ~ b end,
      rshift = function(a, n) return (a >> n) & 0xFFFFFFFF end,
      lshift = function(a, n) return (a << n) & 0xFFFFFFFF end,
    }]])
  if native then
    local ok, t = pcall(native)
    if ok then bitops = t end
  end

  if not bitops.band and rawget(_G, "bit32") then
    local b = rawget(_G, "bit32")
    bitops = {
      band = b.band, bor = b.bor, bxor = b.bxor,
      rshift = b.rshift, lshift = function(a, n) return b.band(b.lshift(a, n), 0xFFFFFFFF) end,
    }
  end

  if not bitops.band then
    -- Pure-arithmetic fallback (Lua 5.1 without bit32). Correct, just slow;
    -- only ever used to build the CRC table + fold bytes, which is cheap.
    local function to_bits(x)
      local t = {}
      for i = 0, 31 do t[i] = math.floor(x / 2 ^ i) % 2 end
      return t
    end
    local function from_bits(t)
      local x = 0
      for i = 0, 31 do x = x + t[i] * 2 ^ i end
      return x
    end
    local function bitwise(a, b, f)
      local ta, tb, tr = to_bits(a), to_bits(b), {}
      for i = 0, 31 do tr[i] = f(ta[i], tb[i]) end
      return from_bits(tr)
    end
    bitops = {
      band = function(a, b) return bitwise(a, b, function(x, y) return (x == 1 and y == 1) and 1 or 0 end) end,
      bor  = function(a, b) return bitwise(a, b, function(x, y) return (x == 1 or y == 1) and 1 or 0 end) end,
      bxor = function(a, b) return bitwise(a, b, function(x, y) return (x ~= y) and 1 or 0 end) end,
      rshift = function(a, n) return math.floor(a / 2 ^ n) % 4294967296 end,
      lshift = function(a, n) return (a * 2 ^ n) % 4294967296 end,
    }
  end
end
M._bitops = bitops -- exposed for the shim self-test

local band, bxor, rshift, lshift = bitops.band, bitops.bxor, bitops.rshift, bitops.lshift

--============================================================================
-- CRC — coverage is payload bytes + trailer_ts bytes (NOT the header). See
-- docs/reference/01-data-format.md §Coverage.
--============================================================================

-- CRC-32C (Castagnoli), reflected poly 0x82F63B78, init/xorout 0xFFFFFFFF.
local CRC32C_TABLE = {}
do
  local POLY = 0x82F63B78
  for n = 0, 255 do
    local c = n
    for _ = 1, 8 do
      if band(c, 1) ~= 0 then c = bxor(rshift(c, 1), POLY) else c = rshift(c, 1) end
    end
    CRC32C_TABLE[n] = c
  end
end

--[[ @brief CRC-32C over a byte string. Returns a 32-bit unsigned integer. ]]--
function M.crc32c(bytes)
  local crc = 0xFFFFFFFF
  for i = 1, #bytes do
    crc = bxor(rshift(crc, 8), CRC32C_TABLE[band(bxor(crc, string.byte(bytes, i)), 0xFF)])
  end
  return band(bxor(crc, 0xFFFFFFFF), 0xFFFFFFFF)
end

--[[ @brief CRC-16-CCITT (poly 0x1021, init 0xFFFF, xorout 0x0000). ]]--
function M.crc16ccitt(bytes)
  local crc = 0xFFFF
  for i = 1, #bytes do
    crc = band(bxor(crc, lshift(string.byte(bytes, i), 8)), 0xFFFF)
    for _ = 1, 8 do
      if band(crc, 0x8000) ~= 0 then
        crc = band(bxor(lshift(crc, 1), 0x1021), 0xFFFF)
      else
        crc = band(lshift(crc, 1), 0xFFFF)
      end
    end
  end
  return band(crc, 0xFFFF)
end

--============================================================================
-- Registries (docs/reference/05-protocol-tlvs.md, RFC-0002, RFC-0004)
--============================================================================
M.TYPE_NAMES = {
  [0x00] = "SENTINEL", [0x01] = "VALUE", [0x02] = "NAME", [0x03] = "DESCRIPTION",
  [0x04] = "SUBSCRIBER", [0x05] = "RESERVED", [0x06] = "PATH", [0x07] = "POINT",
  [0x08] = "ERROR", [0x09] = "STATUS", [0x0A] = "ACL", [0x0B] = "SETTINGS",
  [0x0C] = "TIME", [0x0D] = "ROUTER", [0x0E] = "SPEC", [0x0F] = "FWD",
  [0x10] = "FIELD", [0x11] = "ADVERTISE", [0x12] = "COMPACT", [0x13] = "HANDLE_NACK",
}

-- Structured types (opt.PL SHOULD be 1). Used only for display hints.
M.STRUCTURED = {
  [0x04] = true, [0x06] = true, [0x07] = true, [0x08] = true, [0x09] = true,
  [0x0A] = true, [0x0B] = true, [0x0E] = true, [0x0F] = true, [0x10] = true,
  [0x11] = true, [0x12] = true, [0x13] = true,
}

M.FWD_OP = { [0] = "READ", [1] = "WRITE", [2] = "AWAIT", [3] = "REPLY" }
M.FWD_KIND = { [0] = "RESULT", [1] = "ERROR" }
M.INDEX_MODE = { [0] = "ELEMENT", [1] = "WILDCARD" }

-- tr::<concept>::<error> registry, keyed by the u16 registered code.
M.ERROR_CODES = {
  [0x0001] = "tr::frame::truncated", [0x0002] = "tr::frame::invalid",
  [0x0003] = "tr::frame::crc_fail", [0x0010] = "tr::tlv::nesting_too_deep",
  [0x0020] = "tr::path::not_found", [0x0021] = "tr::path::invalid",
  [0x0022] = "tr::path::in_use", [0x0030] = "tr::schema::type_mismatch",
  [0x0031] = "tr::schema::not_found", [0x0040] = "tr::flow::backpressure",
  [0x0041] = "tr::flow::timeout", [0x0042] = "tr::flow::address_shift_gap",
  [0x0050] = "tr::access::denied", [0x0060] = "tr::transport::down",
  [0x0070] = "tr::version::mismatch",
}

--[[ @brief Human name for a type code, including the open ranges. ]]--
function M.type_name(t)
  local n = M.TYPE_NAMES[t]
  if n then return n end
  if t >= 0x14 and t <= 0x1F then return "core-reserved" end
  if t >= 0x20 and t <= 0x7F then return "future-core-reserved" end
  if t >= 0x80 and t <= 0xFF then return "user-defined" end
  return "unknown"
end

--============================================================================
-- Little-endian readers over a 0-based offset into a Lua byte-string.
--============================================================================
local function u16le(b, off)
  local lo, hi = string.byte(b, off + 1, off + 2)
  return lo + hi * 256
end
local function u32le(b, off)
  local b0, b1, b2, b3 = string.byte(b, off + 1, off + 4)
  return b0 + b1 * 256 + b2 * 65536 + b3 * 16777216
end
-- u64 ns timestamps exceed 2^53, so keep exact hex and a best-effort decimal.
local function u64le_hex(b, off)
  local lo, hi = u32le(b, off), u32le(b, off + 4)
  return string.format("0x%08X%08X", hi, lo)
end
local function i32le(b, off)
  local v = u32le(b, off)
  if v >= 0x80000000 then v = v - 0x100000000 end
  return v
end
local function tohex(b, off, len)
  local t = {}
  for i = off + 1, off + len do t[#t + 1] = string.format("%02X", string.byte(b, i)) end
  return table.concat(t)
end

--============================================================================
-- Pure decoder. Returns a node table (see file header). Never throws on
-- malformed input — it records `err` / `need_more` and decodes what it can.
--============================================================================

--[[ @brief Parse one TLV starting at 0-based `off` in byte-string `b`. ]]--
function M.parse(b, off, depth, maxdepth)
  off = off or 0
  depth = depth or 0
  maxdepth = maxdepth or 64
  local avail = #b - off
  local node = { off = off, opt = {}, children = {} }

  if avail < 4 then
    node.err = "truncated header"
    node.need_more = 4 - avail
    return node
  end

  local t = string.byte(b, off + 1)
  local opt = string.byte(b, off + 2)
  node.type = t
  node.type_name = M.type_name(t)
  node.opt_raw = opt

  -- opt bitfield: R|PL|TS|CR|LL|CW|TF|R  (bit 7 .. bit 0)
  local function bit(pos) return math.floor(opt / (2 ^ pos)) % 2 end
  node.opt = {
    r7 = bit(7), PL = bit(6), TS = bit(5), CR = bit(4),
    LL = bit(3), CW = bit(2), TF = bit(1), r0 = bit(0),
  }

  -- Reserved bits MUST be zero (docs/reference/01 §Options bitfield).
  if node.opt.r7 ~= 0 or node.opt.r0 ~= 0 then
    node.invalid = "reserved opt bit set (tr::frame::invalid)"
  end
  if t == 0x00 then
    node.invalid = "type 0x00 sentinel (tr::frame::invalid)"
  end

  local H = (node.opt.LL == 1) and 6 or 4
  if node.opt.LL == 1 and avail < 6 then
    node.err = "truncated extended header"
    node.need_more = 6 - avail
    node.header_len = 6
    return node
  end
  node.header_len = H
  node.length = (node.opt.LL == 1) and u32le(b, off + 2) or u16le(b, off + 2)

  -- Trailer widths from opt.
  local ts_len = 0
  if node.opt.TS == 1 then ts_len = (node.opt.TF == 1) and 4 or 8 end
  local crc_len = 0
  if node.opt.CR == 1 then crc_len = (node.opt.CW == 1) and 2 or 4 end

  local total = H + node.length + ts_len + crc_len
  node.total_len = total
  node.payload_off = off + H
  node.payload_len = node.length

  if avail < total then
    node.err = "truncated frame"
    node.need_more = total - avail
    return node
  end

  -- Children (structured) vs opaque payload.
  if node.opt.PL == 1 then
    if depth >= maxdepth then
      node.err = "max nesting depth reached"
    else
      local cur = node.payload_off
      local pend = node.payload_off + node.length
      while cur < pend do
        local child = M.parse(b, cur, depth + 1, maxdepth)
        node.children[#node.children + 1] = child
        if child.err or not child.total_len or child.total_len <= 0 then
          node.err = node.err or "malformed child"
          break
        end
        if cur + child.total_len > pend then
          node.err = "child overruns parent payload"
          break
        end
        cur = cur + child.total_len
      end
    end
  else
    node.payload_hex = tohex(b, node.payload_off, node.length)
  end

  -- Trailer.
  node.trailer = { ts = nil, crc = nil }
  local toff = node.payload_off + node.length
  if ts_len > 0 then
    if node.opt.TF == 1 then
      node.trailer.ts = { off = toff, len = 4, form = "rel-i32", value = i32le(b, toff),
                          hex = tohex(b, toff, 4) }
    else
      node.trailer.ts = { off = toff, len = 8, form = "abs-u64", value = u64le_hex(b, toff),
                          hex = tohex(b, toff, 8) }
    end
    toff = toff + ts_len
  end
  if crc_len > 0 then
    local covered = string.sub(b, node.payload_off + 1, node.payload_off + node.length + ts_len)
    local stored, computed, width
    if node.opt.CW == 1 then
      width = "CRC-16-CCITT"
      stored = u16le(b, toff)
      computed = M.crc16ccitt(covered)
    else
      width = "CRC-32C"
      stored = u32le(b, toff)
      computed = M.crc32c(covered)
    end
    node.trailer.crc = {
      off = toff, len = crc_len, width = width,
      stored = stored, computed = computed, ok = (stored == computed),
      stored_hex = string.format((node.opt.CW == 1) and "0x%04X" or "0x%08X", stored),
      computed_hex = string.format((node.opt.CW == 1) and "0x%04X" or "0x%08X", computed),
    }
  end

  M._semantics(b, node)
  return node
end

--[[ @brief Attach type-specific semantics: PATH string, FWD/FIELD, ERROR id. ]]--
function M._semantics(b, node)
  local t = node.type

  if t == 0x06 then -- PATH: join NAME children into /a/b/c
    local segs = {}
    for _, c in ipairs(node.children) do
      if c.type == 0x02 and c.payload_off then
        segs[#segs + 1] = string.sub(b, c.payload_off + 1, c.payload_off + c.length)
      end
    end
    node.path_str = "/" .. table.concat(segs, "/")

  elseif t == 0x0F then -- FWD: VALUE op, PATH dst, PATH src, [VALUE kind (REPLY)]
    local fwd, paths, values = {}, {}, {}
    for _, c in ipairs(node.children) do
      if c.type == 0x01 and c.length >= 1 then
        values[#values + 1] = string.byte(b, c.payload_off + 1)
      elseif c.type == 0x06 then
        paths[#paths + 1] = c.path_str
      end
    end
    fwd.op = values[1]
    fwd.op_name = fwd.op and (M.FWD_OP[fwd.op] or ("op?" .. fwd.op)) or nil
    fwd.dst = paths[1]
    fwd.src = paths[2]
    if fwd.op == 3 and values[2] then -- REPLY carries a kind after src
      fwd.kind = values[2]
      fwd.kind_name = M.FWD_KIND[fwd.kind] or ("kind?" .. fwd.kind)
    end
    node.fwd = fwd

  elseif t == 0x10 then -- FIELD: NAME field_name (+ optional index / index_mode)
    local fld = {}
    for _, c in ipairs(node.children) do
      if c.type == 0x02 and not fld.name then
        fld.name = string.sub(b, c.payload_off + 1, c.payload_off + c.length)
      elseif c.type == 0x01 and c.length == 4 and not fld.index then
        fld.index = u32le(b, c.payload_off)
      end
    end
    node.field = fld

  elseif t == 0x08 then -- ERROR: first child VALUE=registered code | NAME=string form
    local c = node.children[1]
    if c then
      if c.type == 0x01 and c.length >= 2 then
        local code = u16le(b, c.payload_off)
        node.error_id = { form = "code", code = code, name = M.ERROR_CODES[code] or "unregistered" }
      elseif c.type == 0x02 then
        node.error_id = { form = "string",
                          name = string.sub(b, c.payload_off + 1, c.payload_off + c.length) }
      end
    end
  end
end

--[[ @brief One-line human summary of a top-level frame (Info column). ]]--
function M.summary(node)
  if not node.type then return "malformed" end
  local name = node.type_name
  if node.type == 0x0F and node.fwd then
    local s = "FWD " .. (node.fwd.op_name or "?")
    if node.fwd.kind_name then s = s .. " " .. node.fwd.kind_name end
    if node.fwd.dst then s = s .. " dst=" .. node.fwd.dst end
    return s
  elseif node.type == 0x06 and node.path_str then
    return "PATH " .. node.path_str
  elseif node.type == 0x10 and node.field then
    local s = "FIELD :" .. (node.field.name or "?")
    if node.field.index then s = s .. "[" .. node.field.index .. "]" end
    return s
  elseif node.type == 0x08 and node.error_id then
    return "ERROR " .. (node.error_id.name or "?")
  elseif node.type == 0x09 and node.length == 0 then
    return "STATUS OK"
  end
  local s = name .. " len=" .. tostring(node.length or "?")
  if node.trailer and node.trailer.crc then
    s = s .. (node.trailer.crc.ok and " crc=OK" or " crc=BAD")
  end
  if node.invalid then s = s .. " [INVALID]" end
  return s
end

--============================================================================
-- JSON emitter (hand-rolled; used by the standalone test harness).
--============================================================================
local function jstr(s)
  return '"' .. tostring(s):gsub('[\\"]', '\\%0'):gsub('\n', '\\n') .. '"'
end
local function jbool(v) return v and "true" or "false" end

local function node_to_json(n)
  local parts = {}
  parts[#parts + 1] = '"type":' .. tostring(n.type or "null")
  parts[#parts + 1] = '"type_name":' .. jstr(n.type_name or "")
  parts[#parts + 1] = '"opt_raw":' .. tostring(n.opt_raw or "null")
  local o = n.opt or {}
  parts[#parts + 1] = string.format(
    '"opt":{"PL":%d,"TS":%d,"CR":%d,"LL":%d,"CW":%d,"TF":%d,"r7":%d,"r0":%d}',
    o.PL or 0, o.TS or 0, o.CR or 0, o.LL or 0, o.CW or 0, o.TF or 0, o.r7 or 0, o.r0 or 0)
  parts[#parts + 1] = '"length":' .. tostring(n.length or "null")
  parts[#parts + 1] = '"total_len":' .. tostring(n.total_len or "null")
  parts[#parts + 1] = '"payload_hex":' .. jstr(n.payload_hex or "")
  parts[#parts + 1] = '"invalid":' .. (n.invalid and jstr(n.invalid) or "false")
  parts[#parts + 1] = '"err":' .. (n.err and jstr(n.err) or "null")
  parts[#parts + 1] = '"need_more":' .. tostring(n.need_more or "null")

  if n.trailer then
    local tp = {}
    if n.trailer.ts then
      tp[#tp + 1] = string.format('"ts":{"form":%s,"hex":%s,"value":%s}',
        jstr(n.trailer.ts.form), jstr(n.trailer.ts.hex), jstr(tostring(n.trailer.ts.value)))
    else tp[#tp + 1] = '"ts":null' end
    if n.trailer.crc then
      local c = n.trailer.crc
      tp[#tp + 1] = string.format('"crc":{"width":%s,"stored":%s,"computed":%s,"ok":%s}',
        jstr(c.width), jstr(c.stored_hex), jstr(c.computed_hex), jbool(c.ok))
    else tp[#tp + 1] = '"crc":null' end
    parts[#parts + 1] = '"trailer":{' .. table.concat(tp, ",") .. "}"
  end

  if n.path_str then parts[#parts + 1] = '"path_str":' .. jstr(n.path_str) end
  if n.fwd then
    parts[#parts + 1] = string.format(
      '"fwd":{"op":%s,"op_name":%s,"dst":%s,"src":%s,"kind":%s,"kind_name":%s}',
      tostring(n.fwd.op or "null"), jstr(n.fwd.op_name or ""), jstr(n.fwd.dst or ""),
      jstr(n.fwd.src or ""), tostring(n.fwd.kind or "null"), jstr(n.fwd.kind_name or ""))
  end
  if n.field then
    parts[#parts + 1] = string.format('"field":{"name":%s,"index":%s}',
      jstr(n.field.name or ""), tostring(n.field.index or "null"))
  end
  if n.error_id then
    parts[#parts + 1] = string.format('"error_id":{"form":%s,"name":%s,"code":%s}',
      jstr(n.error_id.form), jstr(n.error_id.name or ""), tostring(n.error_id.code or "null"))
  end

  if n.children and #n.children > 0 then
    local cp = {}
    for _, c in ipairs(n.children) do cp[#cp + 1] = node_to_json(c) end
    parts[#parts + 1] = '"children":[' .. table.concat(cp, ",") .. "]"
  else
    parts[#parts + 1] = '"children":[]'
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

--[[ @brief Decode a hex string into one JSON line (with a summary). ]]--
function M.decode_json(hex)
  hex = hex:gsub("%s", "")
  local bytes = {}
  for pair in hex:gmatch("%x%x") do bytes[#bytes + 1] = string.char(tonumber(pair, 16)) end
  local b = table.concat(bytes)
  local n = M.parse(b, 0)
  return '{"summary":' .. jstr(M.summary(n)) .. ',"frame":' .. node_to_json(n) .. "}"
end

--============================================================================
-- Standalone CLI (skipped inside Wireshark, where `arg` is nil).
--============================================================================
if rawget(_G, "arg") and arg[1] then
  if arg[1] == "--decode-json" and arg[2] then
    io.write(M.decode_json(arg[2]) .. "\n")
    os.exit(0)
  elseif arg[1] == "--version" then
    io.write("libtracer.lua " .. M.VERSION .. "\n")
    os.exit(0)
  end
end

--============================================================================
-- Wireshark registration (only when the host provides the `Proto` global).
--============================================================================
if rawget(_G, "Proto") then
  local p = Proto("libtracer", "libtracer protocol")

  local vs_type = {}
  for code, nm in pairs(M.TYPE_NAMES) do vs_type[code] = string.format("%s (0x%02X)", nm, code) end
  local vs_op   = { [0] = "READ", [1] = "WRITE", [2] = "AWAIT", [3] = "REPLY" }
  local vs_kind = { [0] = "RESULT", [1] = "ERROR" }

  local f = {
    type     = ProtoField.uint8("libtracer.type", "Type", base.HEX, vs_type),
    opt      = ProtoField.uint8("libtracer.opt", "Options", base.HEX),
    pl       = ProtoField.uint8("libtracer.opt.pl", "PL (structured)", base.DEC, nil, 0x40),
    ts       = ProtoField.uint8("libtracer.opt.ts", "TS (has timestamp)", base.DEC, nil, 0x20),
    cr       = ProtoField.uint8("libtracer.opt.cr", "CR (has CRC)", base.DEC, nil, 0x10),
    ll       = ProtoField.uint8("libtracer.opt.ll", "LL (u32 length)", base.DEC, nil, 0x08),
    cw       = ProtoField.uint8("libtracer.opt.cw", "CW (CRC-16)", base.DEC, nil, 0x04),
    tf       = ProtoField.uint8("libtracer.opt.tf", "TF (relative TS)", base.DEC, nil, 0x02),
    rsv      = ProtoField.uint8("libtracer.opt.reserved", "Reserved (MUST be 0)", base.HEX, nil, 0x81),
    length   = ProtoField.uint32("libtracer.length", "Length", base.DEC),
    payload  = ProtoField.bytes("libtracer.payload", "Payload"),
    path     = ProtoField.string("libtracer.path", "Path"),
    fwd_op   = ProtoField.uint8("libtracer.fwd.op", "FWD op", base.DEC, vs_op),
    fwd_kind = ProtoField.uint8("libtracer.fwd.kind", "FWD kind", base.DEC, vs_kind),
    fwd_dst  = ProtoField.string("libtracer.fwd.dst", "FWD dst"),
    fwd_src  = ProtoField.string("libtracer.fwd.src", "FWD src"),
    field_nm = ProtoField.string("libtracer.field.name", "Field"),
    err_name = ProtoField.string("libtracer.error", "Error"),
    ts_abs   = ProtoField.uint64("libtracer.trailer.ts_abs", "Wire-time (abs ns)", base.DEC),
    ts_rel   = ProtoField.int32("libtracer.trailer.ts_rel", "Wire-time (rel ns)", base.DEC),
    crc      = ProtoField.uint32("libtracer.trailer.crc", "Trailer CRC", base.HEX),
  }
  local farr = {}
  for _, pf in pairs(f) do farr[#farr + 1] = pf end
  p.fields = farr

  local ef = {
    crc_bad = ProtoExpert.new("libtracer.crc.bad", "Trailer CRC mismatch", expert.group.CHECKSUM, expert.severity.ERROR),
    invalid = ProtoExpert.new("libtracer.invalid", "Invalid frame", expert.group.MALFORMED, expert.severity.WARN),
    trunc   = ProtoExpert.new("libtracer.truncated", "Truncated frame", expert.group.MALFORMED, expert.severity.WARN),
  }
  p.experts = { ef.crc_bad, ef.invalid, ef.trunc }

  local pref_ws  = 80    -- WebSocket TCP port (strawberry-fw serves on :80)
  local pref_tcp = 0     -- raw-TCP length_prefix_framer port; 0 = disabled
  p.prefs.ws_port  = Pref.uint("WebSocket port", pref_ws,  "TCP port of the WebSocket carrying libtracer (0 = off)")
  p.prefs.tcp_port = Pref.uint("Raw TCP port", pref_tcp,  "TCP port for raw length-prefixed libtracer (0 = off)")
  p.prefs.heur     = Pref.bool("Heuristic TCP detection", true, "Try to auto-detect libtracer on any TCP stream")

  -- Recursively render a decoded node onto the proto tree. Every tvb() range is
  -- range-checked: a malformed/truncated (nested) node may claim bytes past the
  -- captured buffer, and tvb(off, len) throws on an out-of-range slice.
  local function add_node(node, tvb, tree)
    local blen = tvb:len()
    local function have(off, len) return off >= 0 and (off + len) <= blen end

    -- Clamp the node's own highlight range.
    local nlen = node.total_len or (blen - node.off)
    if node.off + nlen > blen then nlen = blen - node.off end
    if nlen < 1 then nlen = 1 end
    local sub = tree:add(p, tvb(node.off, nlen), "libtracer: " .. M.summary(node))

    if node.invalid then sub:add_proto_expert_info(ef.invalid, node.invalid) end
    if node.err then sub:add_proto_expert_info(ef.trunc, node.err) end
    if not node.type then return node.total_len or nlen end  -- header never read

    sub:add(f.type, tvb(node.off, 1))
    if have(node.off + 1, 1) then
      local optitem = sub:add(f.opt, tvb(node.off + 1, 1))
      for _, sf in ipairs({ f.pl, f.ts, f.cr, f.ll, f.cw, f.tf, f.rsv }) do
        optitem:add(sf, tvb(node.off + 1, 1))
      end
    end
    local lwidth = (node.header_len == 6) and 4 or 2
    if have(node.off + 2, lwidth) then sub:add_le(f.length, tvb(node.off + 2, lwidth)) end

    -- Derived/semantic fields: *generated* items over the frame range, explicit
    -- value (they are not a verbatim slice of the bytes).
    local frange = tvb(node.off, nlen)
    local function gen(field, value) sub:add(field, frange, value):set_generated() end
    if node.path_str then gen(f.path, node.path_str) end
    if node.fwd then
      if node.fwd.op then gen(f.fwd_op, node.fwd.op) end
      if node.fwd.kind then gen(f.fwd_kind, node.fwd.kind) end
      if node.fwd.dst then gen(f.fwd_dst, node.fwd.dst) end
      if node.fwd.src then gen(f.fwd_src, node.fwd.src) end
    end
    if node.field and node.field.name then gen(f.field_nm, node.field.name) end
    if node.error_id then gen(f.err_name, node.error_id.name) end

    if node.opt.PL == 1 then
      for _, c in ipairs(node.children) do add_node(c, tvb, sub) end
    elseif (node.length or 0) > 0 and have(node.payload_off, node.length) then
      sub:add(f.payload, tvb(node.payload_off, node.length))
    end

    local tr = node.trailer
    if tr and tr.ts and have(tr.ts.off, tr.ts.len) then
      if tr.ts.form == "abs-u64" then sub:add_le(f.ts_abs, tvb(tr.ts.off, 8))
      else sub:add_le(f.ts_rel, tvb(tr.ts.off, 4)) end
    end
    if tr and tr.crc and have(tr.crc.off, tr.crc.len) then
      local c = tr.crc
      local ci = sub:add_le(f.crc, tvb(c.off, c.len))
      ci:append_text(c.ok and " [OK]" or (" [BAD, computed " .. c.computed_hex .. "]"))
      if not c.ok then sub:add_proto_expert_info(ef.crc_bad, "expected " .. c.computed_hex) end
    end
    return node.total_len or nlen
  end

  -- Core dissector: walk every concatenated frame in the buffer.
  function p.dissector(tvb, pinfo, tree)
    local blen = tvb:len()
    if blen < 4 then return 0 end
    local bytes = tvb:raw()
    local off, count = 0, 0
    local first_summary = nil
    while off + 4 <= blen do
      local node = M.parse(bytes, off)
      if node.need_more then
        -- Ask for reassembly (stream transports only; harmless on datagrams).
        pinfo.desegment_offset = off
        pinfo.desegment_len = node.need_more
        break
      end
      if not node.total_len or node.total_len <= 0 then break end
      add_node(node, tvb, tree)
      first_summary = first_summary or M.summary(node)
      off = off + node.total_len
      count = count + 1
    end
    if count > 0 then
      pinfo.cols.protocol = "libtracer"
      pinfo.cols.info:set(first_summary .. (count > 1 and (" (+" .. (count - 1) .. " more)") or ""))
    end
    return off
  end

  -- Heuristic: a strong-enough signal to claim a TCP/WS payload as libtracer.
  local function heuristic(tvb, pinfo, tree)
    if not p.prefs.heur then return false end
    if tvb:len() < 4 then return false end
    local node = M.parse(tvb:raw(), 0)
    if node.invalid or not node.total_len then return false end
    if node.type == 0 or node.type > 0x13 then return false end   -- must be a known code
    if node.opt.r7 ~= 0 or node.opt.r0 ~= 0 then return false end
    -- A verified CRC is near-conclusive; otherwise require an exact frame fit.
    local strong = (node.trailer and node.trailer.crc and node.trailer.crc.ok)
      or (node.total_len == tvb:len())
    if not strong then return false end
    p.dissector(tvb, pinfo, tree)
    return true
  end

  -- Registration. Ports are preferences so a deployment can retarget them.
  -- Every table lookup is guarded: not all Wireshark builds expose "ws.port".
  local function bind(table_name, port)
    if not port or port <= 0 then return end
    pcall(function() DissectorTable.get(table_name):add(port, p) end)
  end
  local function apply_prefs()
    bind("ws.port", p.prefs.ws_port)   -- WebSocket payload by TCP port
    bind("tcp.port", p.prefs.tcp_port) -- raw length-prefixed libtracer
  end
  p.prefs_changed = apply_prefs
  apply_prefs()
  pcall(function() p:register_heuristic("tcp", heuristic) end)
  pcall(function() p:register_heuristic("ws", heuristic) end)
end

_G.libtracer = M
return M
