#!/usr/bin/env python3
# nv_flr_check.py - READ-ONLY preflight for a Function Level Reset (tools/nv_e3_flr.py), after nv-uefi-unlocker's checklist:
# is the function in D0, does the device advertise FLR, are Transactions Pending clear. Reads PCI config space only, through
# the same TinyGPU dext socket nv_e3_flr.py uses. It never writes a register and never maps BAR0.
#   python3 tools/nv_flr_check.py            report, exit 0 if FLR looks safe, 1 if something warns, 2 if no card
#   python3 tools/nv_flr_check.py --selftest offline check of the decoding against a fake config space (no card)
import os, sys

D_NAMES = {0: "D0", 1: "D1", 2: "D2", 3: "D3hot"}
EXT_CAP_NAMES = {0x01: "AER", 0x02: "VC", 0x03: "DSN", 0x04: "PB", 0x0b: "VSEC", 0x0d: "ACS", 0x0f: "ATS", 0x10: "SR-IOV",
                 0x15: "Resizable BAR", 0x18: "LTR", 0x19: "SecPCIe", 0x1e: "L1 PM Substates", 0x23: "DVSEC"}

def find_cap(cfg, cap_id):
  """Offset of a legacy capability in config space, or None. cfg(off, size) -> int."""
  if not cfg(0x06, 2) & 0x10: return None   # Status bit 4: capabilities list present
  ptr, seen = cfg(0x34, 1) & 0xfc, set()
  while ptr and ptr not in seen:
    seen.add(ptr)
    if cfg(ptr, 1) == cap_id: return ptr
    ptr = cfg(ptr + 1, 1) & 0xfc
  return None

def ext_caps(cfg):
  """[(id, version, offset)] from the extended capability list at 0x100 (stops on an absent or looping list)."""
  out, off, seen = [], 0x100, set()
  while off and off not in seen and off < 0x1000:
    seen.add(off)
    h = cfg(off, 4)
    if h in (0, 0xffffffff): break
    out.append((h & 0xffff, (h >> 16) & 0xf, off))
    off = (h >> 20) & 0xffc
  return out

def check(cfg, say=print):
  """Decode and report. Returns the list of warnings; empty means FLR looks safe to issue."""
  warn = []
  vid_did = cfg(0x00, 4)
  say(f"vendor/device       : 0x{vid_did & 0xffff:04x}:0x{vid_did >> 16:04x}")
  if vid_did in (0, 0xffffffff): return ["config space reads as no device (all ones/zero)"]
  say(f"PCI Command         : 0x{cfg(0x04, 2):04x}  (bus master {'ON' if cfg(0x04, 2) & 4 else 'off'}, memory decode {'ON' if cfg(0x04, 2) & 2 else 'off'})")
  sta = cfg(0x06, 2)
  say(f"PCI Status (0x06)   : 0x{sta:04x}  (cap list {'yes' if sta & 0x10 else 'no'}, master abort {'SET' if sta & 0x2000 else 'clear'}, "
      f"target abort rx {'SET' if sta & 0x1000 else 'clear'}, sig target abort {'SET' if sta & 0x0800 else 'clear'}, "
      f"parity err {'SET' if sta & 0x0100 else 'clear'}, sys err {'SET' if sta & 0x4000 else 'clear'})")
  # 1. power state
  pm = find_cap(cfg, 0x01)
  if pm is None: say("PMCSR               : no Power Management capability"); warn.append("no PM capability: D-state unknown")
  else:
    pmcsr = cfg(pm + 4, 2); d = pmcsr & 3
    say(f"PMCSR (cap@0x{pm:02x}+4) : 0x{pmcsr:04x}  -> {D_NAMES[d]}")
    if d != 0: warn.append(f"function is in {D_NAMES[d]}, not D0: an FLR from a low-power state is undefined (force D0 first)")
  # 2-3, 5. PCIe capability
  ex = find_cap(cfg, 0x10)
  if ex is None: say("PCIe capability     : absent"); warn.append("no PCIe capability: cannot check FLR support")
  else:
    caps = cfg(ex + 4, 4); ctl = cfg(ex + 8, 2); dsta = cfg(ex + 0xa, 2)
    flr = bool(caps & (1 << 28))
    say(f"DevCap (cap@0x{ex:02x}+4)  : 0x{caps:08x}  -> FLR {'supported' if flr else 'NOT supported'} (bit 28)")
    if not flr: warn.append("DevCap bit 28 clear: the device does not advertise FLR")
    say(f"DevCtl (cap@0x{ex:02x}+8)  : 0x{ctl:04x}  (error reporting en: corr {ctl & 1}, nonfatal {(ctl >> 1) & 1}, fatal {(ctl >> 2) & 1}, "
        f"unsupported-req {(ctl >> 3) & 1}; relaxed ordering {(ctl >> 4) & 1}; max payload code {(ctl >> 5) & 7}; "
        f"ext tag {(ctl >> 8) & 1}; no-snoop {(ctl >> 11) & 1}; max read req code {(ctl >> 12) & 7}; initiate-FLR bit {(ctl >> 15) & 1})")
    pend = bool(dsta & (1 << 5))
    say(f"DevSta (cap@0x{ex:02x}+0xa): 0x{dsta:04x}  -> transactions pending {'YES' if pend else 'no'} (bit 5); "
        f"error bits corr {dsta & 1}, nonfatal {(dsta >> 1) & 1}, fatal {(dsta >> 2) & 1}, unsupported-req {(dsta >> 3) & 1}")
    if pend: warn.append("Transactions Pending is set: the device is still mastering; wait for it to clear (nv-uefi-unlocker: then hold 100 ms quiet) before an FLR")
    if dsta & 0xf: say("  (DevSta error bits are write-1-to-clear; this tool does not clear them)")
  # 5. extended capabilities
  ec = ext_caps(cfg)
  say("extended caps       : " + (", ".join(f"{EXT_CAP_NAMES.get(i, f'0x{i:04x}')} v{v}@0x{o:03x}" for i, v, o in ec) if ec else "none readable"))
  return warn

def _selftest():
  mem = {0x00: 0x252010de, 0x04: 0x0006, 0x06: 0x0010, 0x34: 0x60, 0x60: 0x01, 0x61: 0x68, 0x64: 0x0000,
         0x68: 0x10, 0x69: 0x00, 0x6c: 0x10008fe0 | (1 << 28), 0x70: 0x2810, 0x72: 0x0020, 0x100: 0x00010001 | (0x140 << 20), 0x140: 0x00010019}
  def fake(off, size=4):
    if size == 4: return sum(mem.get(off + i, 0) << (8 * i) for i in range(4)) if off % 4 else mem.get(off, 0)
    return mem.get(off, 0) & ((1 << (8 * size)) - 1)
  # byte-granular fake: store dwords/words as given, answer exact-offset lookups
  w = check(fake, say=lambda *_: None)
  assert len(w) == 1 and "Transactions Pending" in w[0], w
  mem[0x72] = 0; mem[0x64] = 0x0003
  w = check(fake, say=lambda *_: None)
  assert len(w) == 1 and "D3hot" in w[0], w
  mem[0x64] = 0; mem[0x6c] = 0
  assert any("FLR" in x for x in check(fake, say=lambda *_: None))
  assert check(lambda o, s=4: 0xffffffff & ((1 << (8 * s)) - 1), say=lambda *_: None)
  print("selftest ok")

def main():
  if "--selftest" in sys.argv: return _selftest()
  sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
  try:
    from nvmini import MiniNVDev   # imports tinygrad; fails here when the venv is not active
    d = MiniNVDev(timeout=20, map_bar0=False)   # no BAR0 mapping: config space only
  except Exception as e:
    print(f"nv_flr_check: no card to inspect ({type(e).__name__}: {e})"); return 2
  try:
    warn = check(d.cfg)
  except Exception as e:
    print(f"nv_flr_check: config read failed ({type(e).__name__}: {e})"); return 2
  finally:
    try: d.close()
    except Exception: pass
  print("verdict: " + ("FLR preflight clean (read-only; nothing was written)" if not warn else "WARN"))
  for x in warn: print("  WARN:", x)
  return 1 if warn else 0

if __name__ == "__main__": sys.exit(main())
