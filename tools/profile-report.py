"""Summarize logs without treating invalid timestamps as causal measurements."""
import argparse
import json
import re
from pathlib import Path


def report(text):
    rates = []
    for line in text.splitlines():
        match = re.search(r'\|\s*(pp\d+|tg\d+)\s*\|\s*([\d.]+)\s*±\s*([\d.]+)', line)
        if match:
            rates.append(dict(test=match[1], tokens_per_second=float(match[2]), spread=float(match[3])))
    raw = []
    for tail, head in re.findall(r'refill \d+ raw: tail (\d+)\s+head (\d+)', text):
        tail, head = int(tail), int(head)
        raw.append(tail > 0 and 0 < head - tail <= 100_000_000)
    discarded = re.findall(r'(\d+) refill measurements were discarded', text)
    negative = [line.strip() for line in text.splitlines() if re.search(r'(?<![\w.])-\d+(?:\.\d+)? us', line)]
    warnings = []
    if raw and not all(raw): warnings.append('Invalid raw timestamp pairs in the printed sample.')
    if discarded and int(discarded[-1]): warnings.append('Profiler discarded refill samples.')
    if negative: warnings.append('Negative derived durations: do not use the timing breakdown for causal attribution.')
    if 'where the processor' not in text: warnings.append('No launch profile present.')
    return dict(throughput=rates, driver_builds=re.findall(r'libtinynv build ([\w-]+)', text),
                printed_raw_samples=len(raw), invalid_printed_samples=raw.count(False),
                discarded_refills=int(discarded[-1]) if discarded else None,
                negative_duration_lines=negative, warnings=warnings,
                timing_validity='needs_review' if warnings else 'no_obvious_anomaly_not_clock_validation',
                note='Throughput rows do not prove correctness or successful process exit. Printed raw samples are not the full population.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    args = p.parse_args()
    print(json.dumps(report(args.log.read_text(errors='replace')), indent=2))
