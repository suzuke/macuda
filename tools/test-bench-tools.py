import importlib.util
import unittest
from pathlib import Path
from unittest.mock import patch


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name+'.py'))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

profile = load('profile-report')
ab = load('ab-bench')


class ToolsTests(unittest.TestCase):
    def test_bad_timestamps_do_not_erase_throughput(self):
        text = '''where the processor's time went
| model | tg128 | 51.76 ± 0.00 |
refill 0 raw: tail 0 head 123
refill 1 raw: tail 200 head 100
remaining -2157.2 us
87 refill measurements were discarded'''
        result = profile.report(text)
        self.assertEqual(result['throughput'][0]['tokens_per_second'], 51.76)
        self.assertEqual(result['invalid_printed_samples'], 2)
        self.assertEqual(result['discarded_refills'], 87)
        self.assertEqual(result['timing_validity'], 'needs_review')
    def test_no_profile_does_not_imply_valid_clock(self):
        self.assertEqual(profile.report('')['timing_validity'], 'needs_review')
    def test_valid_sample_is_not_clock_proof(self):
        result = profile.report('where the processor\nrefill 0 raw: tail 100 head 200')
        self.assertEqual(result['timing_validity'], 'no_obvious_anomaly_not_clock_validation')
    def test_abba_changes_only_one_variable(self):
        steps = ab.plan(Path('/model'), Path('/binary'), 'TINYNV_DELTA_DELIVERY', '0', '1')
        self.assertEqual(''.join(s['arm'] for s in steps), 'ABBA')
        self.assertEqual(steps[0]['command'], steps[1]['command'])
        self.assertEqual([k for k in steps[0]['env'] if steps[0]['env'][k] != steps[1]['env'][k]], ['TINYNV_DELTA_DELIVERY'])
        self.assertTrue(all(s['env']['QUIESCE']=='1' for s in steps))
    def test_reject_unsupported_or_identical_knobs(self):
        for knob,a,b in [('TINYNV_SYNC','0','1'),('TINYNV_CHAIN_DEPTH','128','128'),('TINYNV_CHAIN_DEPTH','0','64')]:
            with self.assertRaises(ValueError): ab.plan(Path('/m'),Path('/b'),knob,a,b)
    def test_plan_only_never_runs_subprocess_or_reads_model(self):
        with patch('sys.argv',['ab-bench.py','--model','/nonexistent']), patch('builtins.print'), \
             patch.object(ab.subprocess,'run',side_effect=AssertionError('hardware call')), \
             patch.object(ab,'digest',side_effect=AssertionError('model read')):
            ab.main()

unittest.main()
