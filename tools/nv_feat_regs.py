# Read FEAT / speed-select override registers to check tensor-core throttle state.
# Read-only: no writes, no reset. Requires TinyGPU server running.
#
# Ampere (GA10x) register map (from nv-uefi-unlocker reverse engineering):
#   FEAT_OVR_PLM   0x00823804  - privilege level mask; 0xffffff8f = locked (L3 only), 0xffffffff = open
#   FEAT_READOUT   0x00823814  - NV_FUSE_FEATURE_READOUT (only public name in this block)
#   SS0            0x0082381c  - speed-select override 0; each nibble = [ovr_en | 3-bit rate], 0x8 = full speed
#   SS1            0x00823820  - speed-select override 1
#
# Turing (TU10x) uses a different block at 0x004096xx.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvmini import MiniNVDev

d = MiniNVDev()
s = d.state()
if s['mmio_all_ones']:
    print("MMIO reads all-ones: link/BAR problem"); d.close(); sys.exit(2)

arch = int(s['arch'], 16)
print(f"chip: arch=0x{arch:02x} impl={s['impl']}  boot0={s['boot0']}  GSP={'resident' if s['gsp_resident'] else 'cold'}")

if arch == 0x17:  # Ampere GA10x
    regs = {
        'FEAT_OVR_PLM':  0x00823804,
        'FEAT_READOUT':  0x00823814,
        'SS0':           0x0082381c,
        'SS1':           0x00823820,
    }
    # additional readout at 0x00400080 mentioned in uplift doc
    extra = {'FEAT_READOUT_1': 0x00400080}
elif arch == 0x16:  # Turing TU10x
    regs = {
        'TU_FEAT_0':     0x00409650,
        'TU_FEAT_1':     0x00409664,
        'TU_FEAT_READOUT': 0x00409668,
        'TU_FEAT_2':     0x0040966c,
    }
    extra = {}
elif arch == 0x19:  # Ada AD10x
    regs = {
        'FEAT_OVR_PLM':  0x00823804,
        'FEAT_READOUT':  0x00823814,
        'SS0':           0x0082381c,
        'SS1':           0x00823820,
    }
    extra = {'FEAT_READOUT_1': 0x00400080}
else:
    print(f"unknown arch 0x{arch:02x}, dumping Ampere offsets anyway")
    regs = {
        'FEAT_OVR_PLM':  0x00823804,
        'FEAT_READOUT':  0x00823814,
        'SS0':           0x0082381c,
        'SS1':           0x00823820,
    }
    extra = {}

print("\n--- FEAT / speed-select registers ---")
for name, addr in regs.items():
    val = d.rreg(addr)
    print(f"  {name:20s}  [0x{addr:08x}] = 0x{val:08x}")

if extra:
    print("\n--- extra readout ---")
    for name, addr in extra.items():
        val = d.rreg(addr)
        print(f"  {name:20s}  [0x{addr:08x}] = 0x{val:08x}")

# witness / canary: PBUS scratch 0x20 and 0x21 (cleared by FLR/SBR/power-off)
print("\n--- PBUS scratch canary (cleared on FLR/SBR/power-off) ---")
for idx in (0x20, 0x21):
    addr = 0x00001400 + idx * 4
    val = d.rreg(addr)
    print(f"  VBIOS_SCRATCH[0x{idx:02x}]  [0x{addr:08x}] = 0x{val:08x}")

# interpret PLM for Ampere/Ada
if arch in (0x17, 0x19):
    plm = d.rreg(0x00823804)
    ss0 = d.rreg(0x0082381c)
    ss1 = d.rreg(0x00823820)
    print("\n--- interpretation ---")
    if plm == 0xffffff8f:
        print("  PLM = 0xffffff8f → LOCKED (only L3/firmware can write SS)")
    elif plm == 0xffffffff:
        print("  PLM = 0xffffffff → OPEN (host can write SS)")
    else:
        print(f"  PLM = 0x{plm:08x} → unexpected value")
    if ss0 == 0 and ss1 == 0:
        print("  SS0/SS1 = 0 → no override active (fuse defaults, likely throttled)")
    elif ss0 == 0x88888888 and ss1 == 0x8:
        print("  SS0=0x88888888 SS1=0x8 → full-speed override ACTIVE (unlocked)")
    else:
        print(f"  SS0=0x{ss0:08x} SS1=0x{ss1:08x} → partial or card-specific override")

d.close()
