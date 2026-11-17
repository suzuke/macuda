"""Plan an ABBA comparison by default. --execute explicitly enables GPU work."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('profile_report', ROOT / 'tools/profile-report.py')
parser_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser_module)
KNOBS = {'TINYNV_DELTA_DELIVERY': ('0', '1'), 'TINYNV_CHAIN_DEPTH': ('32', '64', '128')}


def plan(model, binary, knob, a, b, profile=False):
    if knob not in KNOBS or a not in KNOBS[knob] or b not in KNOBS[knob] or a == b:
        raise ValueError('Choose different A/B values from the supported knob values.')
    return [dict(arm=arm, env={knob: a if arm == 'A' else b, 'TINYNV_LAUNCH_PROFILE': str(int(profile)), 'QUIESCE': '1'},
                 command=['sh', str(ROOT/'tools/nv_shim_step.sh'), 'A', 'probe', str(binary),
                          '-m', str(model), '-ngl', '99', '-p', '256', '-n', '128', '-r', '3'])
            for arm in 'ABBA']


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''): h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--binary', type=Path, default=ROOT/'cuda-shim/build/bin-sm_86/llama-bench-null')
    p.add_argument('--knob', choices=KNOBS, default='TINYNV_DELTA_DELIVERY')
    p.add_argument('--a', default='0'); p.add_argument('--b', default='1')
    p.add_argument('--profile', action='store_true', help='Enable instrumentation in BOTH arms; compare separately from ordinary runs.')
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    model, binary = args.model.resolve(), args.binary.resolve()
    steps = plan(model, binary, args.knob, args.a, args.b, args.profile)
    if not args.execute:
        print(json.dumps(dict(gpu_work=False, steps=steps), indent=2)); return
    if not model.is_file() or not os.access(binary, os.X_OK): p.error('Missing model or executable.')
    # A separate scheduler lock prevents concurrent runs of THIS tool, not other GPU clients.
    with (ROOT/'.ab-bench.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        out = ROOT/'logs'/('ab-'+time.strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
        out.mkdir(parents=True)
        env = {k:v for k,v in os.environ.items() if not k.startswith(('TINYNV_', 'TINYCUDART_', 'TINYCUBLAS_', 'GGML_CUDA_', 'TINYCC_'))
               and k not in ('DRY','GMALLOC','EGPU_ROOT','TREE','TESTBIN','DEXT_SEEN','DART_STALE','MACUDA_SHARED_ROOT','GPU_LOCK')}
        env['EGPU_ROOT'] = str(ROOT)
        manifest = dict(model=str(model), model_sha256=digest(model), binary=str(binary), binary_sha256=digest(binary),
                        source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                        source_status=subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),
                        profile=args.profile, steps=steps, runs=[])
        def save(): (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        save()
        for number, step in enumerate(steps):
            runenv = env | step['env']
            before = set((ROOT/'logs').glob('shim-probe-*.log'))
            summary = out/f'{number}-{step["arm"]}.txt'
            with summary.open('w') as f:
                result = subprocess.run(step['command'],cwd=ROOT,env=runenv,stdout=f,stderr=subprocess.STDOUT)
            fresh = set((ROOT/'logs').glob('shim-probe-*.log'))-before
            text = summary.read_text(errors='replace')
            raw = next(iter(fresh)) if len(fresh)==1 else None
            parsed = parser_module.report(raw.read_text(errors='replace')) if raw else {}
            post = subprocess.run(['sh','tools/preflight.sh'],cwd=ROOT,env=runenv,capture_output=True,text=True)
            (out/f'{number}-postflight.txt').write_text(post.stdout+post.stderr)
            valid = (result.returncode==0 and 'E3 OK:' in text and 'quiesced: verdict:' in text
                     and post.returncode==0 and raw is not None and len(parsed.get('throughput',[]))==2)
            manifest['runs'].append(dict(arm=step['arm'],exit_code=result.returncode,valid=valid,
                                        raw_log=str(raw) if raw else None, report=parsed,load_average=os.getloadavg()))
            save()
            if not valid: raise SystemExit(f'Run failed validation; stopped. Inspect {out}')
        averages = {}
        for arm in 'AB':
            averages[arm] = {metric: statistics.mean(row['tokens_per_second'] for run in manifest['runs'] if run['arm']==arm
                          for row in run['report']['throughput'] if row['test']==metric) for metric in ('pp256','tg128')}
        manifest['arm_mean_tokens_per_second'] = averages
        manifest['note'] = 'Performance comparison only. Validate outputs/operators before adopting a faster setting. ABBA does not eliminate all host-load drift.'
        save(); print(out); print(json.dumps(averages,indent=2))


if __name__ == '__main__': main()
