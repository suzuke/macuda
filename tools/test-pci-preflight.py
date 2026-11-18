import importlib.util
import unittest
spec = importlib.util.spec_from_file_location('probe', __file__.replace('test-pci-preflight.py','pci-preflight.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class DetectionTests(unittest.TestCase):
    def node(self):
        return {'vendor-id': bytes.fromhex('de100000'), 'device-id': bytes.fromhex('44250000'), 'class-code': bytes.fromhex('00000300'), 'IOPCIResourced': True, 'IOPCIExpressLinkStatus': 4164, 'IOPCIExpressLinkCapabilities': 4541700}
    def test_connected(self):
        self.assertEqual(m.inspect([self.node()])[0], '0x2544 x4 16.0 GT/s up')
    def test_missing_or_ambiguous(self):
        for nodes in ([], [self.node(), self.node()]):
            with self.assertRaises(ValueError): m.inspect(nodes)
    def test_invalid_states(self):
        for changes in ({'IOPCIExpressLinkStatus':None}, {'IOPCIExpressLinkStatus':0}, {'IOPCIExpressLinkStatus':4164 | 2048}, {'IOPCIResourced':False}, {'IOPCIExpressLinkCapabilities':None}, {'IOPCIExpressLinkCapabilities':1<<20}):
            n=self.node(); n.update(changes)
            self.assertTrue(m.inspect([n])[0].endswith('unknown'), changes)
    def test_dll_active(self):
        n=self.node(); n.update(IOPCIExpressLinkCapabilities=1<<20, IOPCIExpressLinkStatus=4164 | 8192)
        self.assertTrue(m.inspect([n])[0].endswith('up'))
    def test_audio_excluded(self):
        n=self.node(); n['class-code']=bytes.fromhex('00030400')
        with self.assertRaises(ValueError): m.inspect([n])
class ArchTests(unittest.TestCase):
    def test_known_cards(self):
        for devid, arch in ((0x2544, 'sm_86'), (0x2504, 'sm_86'), (0x2204, 'sm_86'), (0x2684, 'sm_89'),
                            (0x2b85, 'sm_120a'), (0x2c02, 'sm_120a'), (0x2d04, 'sm_120a')):
            self.assertEqual(m.arch_for(devid), arch, hex(devid))
    def test_refused(self):
        for devid in (0x2330, 0x2901, 0x1e04, 0x2e12, 0x20b0):
            with self.assertRaises(ValueError): m.arch_for(devid)
unittest.main()
