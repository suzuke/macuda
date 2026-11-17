"""Read an exact NVIDIA display node from an ioreg XML snapshot; never open hardware."""
import plistlib
import sys


def inspect(nodes):
    found = []
    def walk(items):
        for node in items:
            integer = lambda key: int.from_bytes(node.get(key, b''), 'little')
            if integer('vendor-id') == 0x10de and integer('class-code') >> 16 == 3:
                found.append(node)
            walk(node.get('IORegistryEntryChildren', []))
    walk(nodes)
    if len(found) != 1:
        raise ValueError(f'expected one NVIDIA display node, found {len(found)}')
    node = found[0]
    status = node.get('IOPCIExpressLinkStatus')
    caps = node.get('IOPCIExpressLinkCapabilities')
    resourced = node.get('IOPCIResourced') is True
    speed = status & 15 if isinstance(status, int) else 0
    width = (status >> 4) & 63 if isinstance(status, int) else 0
    # PCIe Link Status: training bit 11, DLL active bit 13. DLL active
    # is mandatory only when Link Capabilities advertises reporting (bit 20).
    valid = (isinstance(status, int) and isinstance(caps, int)
             and 1 <= speed <= 6 and width in (1, 2, 4, 8, 12, 16, 32)
             and not status & (1 << 11)
             and (not caps & (1 << 20) or bool(status & (1 << 13))))
    devid = int.from_bytes(node['device-id'], 'little')
    rate = {1:'2.5', 2:'5.0', 3:'8.0', 4:'16.0', 5:'32.0', 6:'64.0'}.get(speed, '?')
    return f'0x{devid:04x} x{width} {rate} GT/s {"up" if valid and resourced else "unknown"}', node


if __name__ == '__main__':
    try:
        result, node = inspect(plistlib.loads(sys.stdin.buffer.read()))
        if '--nub' in sys.argv:
            if node.get('IOPCIResourced') is True:
                print('"IOPCIResourced" = Yes')
            print('"IODEXTMatchCount" =', node.get('IODEXTMatchCount', 0))
        else:
            print(result)
    except (ValueError, KeyError, TypeError, plistlib.InvalidFileException) as error:
        print(f'PCI detection failed: {error}', file=sys.stderr)
        sys.exit(1)
