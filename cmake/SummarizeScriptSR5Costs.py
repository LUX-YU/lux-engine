"""Validate SR-5 paired workloads; totals and unit costs include the complete measured batch."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics

DEFAULT = ['cpp-update-10k', 'lua-update-10k', 'lua-ability-10k', 'lua-coroutine-10k',
           'lua-event-10k', 'flow-update-10k', 'flow-event-10k', 'event-fanout-10k']
WORK = ('size', 'seed', 'sample', 'active_instances', 'calls', 'ability_calls', 'events', 'suspensions',
        'resumes', 'continuations', 'awaitables', 'event_waiters', 'queue_depth', 'external_queue_depth',
        'external_completion_capacity_failures', 'lifecycle_begins', 'lifecycle_ends', 'checksum', 'started', 'completed')

def workload(case, manifest, rows):
    args = manifest['arguments']
    size = int(args[args.index('--size') + 1])
    warmup = int(args[args.index('--warmups') + 1])
    frames = int(args[args.index('--frames') + 1])
    budget = int(args[args.index('--resume-budget') + 1])
    if case == 'event-fanout-10k':
        assert len(rows) == 1 + (size + budget - 1) // budget
        for i, row in enumerate(rows):
            assert int(row['calls']) == size and int(row['resumes']) == min(i * budget, size)
            assert int(row['queue_depth']) == max(size - i * budget, 0)
            assert int(row['events']) == int(i == 0)
        operations = {'completions': size, 'resumes': size, 'occurrences': 1}
    else:
        assert len(rows) == frames
        for i, row in enumerate(rows):
            elapsed = warmup + i + 1
            assert int(row['active_instances']) == size and int(row['size']) == size
            assert int(row['external_queue_depth']) == 0 if 'external_queue_depth' in row else True
            if case == 'cpp-update-10k':
                assert int(row['calls']) == size and int(row['queue_depth']) == 0
            elif case in ('lua-update-10k', 'lua-ability-10k', 'lua-event-10k'):
                assert int(row['calls']) == elapsed * size and int(row['queue_depth']) == 0
                if case == 'lua-ability-10k': assert int(row['ability_calls']) == elapsed * size
                if case == 'lua-event-10k': assert int(row['resumes']) == elapsed * size and int(row['events']) == 1
            elif case == 'flow-update-10k':
                assert int(row['calls']) == 2 * elapsed * size and int(row['resumes']) == 0
                assert int(row['queue_depth']) == 0
            elif case == 'flow-event-10k':
                assert int(row['calls']) == elapsed * budget and int(row['resumes']) == elapsed * budget
                assert int(row['started']) == size + (elapsed - 1) * budget
                assert int(row['queue_depth']) == size - budget
            elif case == 'lua-coroutine-10k':
                assert int(row['calls']) == size + (elapsed - 1) * budget
                assert int(row['resumes']) == elapsed * budget and int(row['queue_depth']) == size - budget
            else: raise ValueError(case)
        actual = frames * (budget if case in ('flow-event-10k', 'lua-coroutine-10k') else size)
        operations = {'hook_occurrences': frames, 'hook_candidates': frames * size, 'actual_new_calls': actual}
        if case in ('lua-event-10k', 'flow-event-10k', 'lua-coroutine-10k'): operations['resumes'] = actual
        if case in ('lua-event-10k', 'flow-event-10k'): operations['occurrences'] = frames
        if case == 'lua-ability-10k': operations['provider_calls'] = actual
        if case == 'flow-update-10k': operations['provider_calls'] = 2 * actual
        if case == 'flow-event-10k': operations['provider_calls'] = actual
    return operations

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True)
    parser.add_argument('--expect', nargs='+', default=DEFAULT)
    args = parser.parse_args()
    root = Path(args.root)
    manifests = json.loads((root / 'runs.json').read_text(encoding='utf-8-sig'))
    assert len(manifests) == 10 * len(args.expect) and {m['case'] for m in manifests} == set(args.expect)
    reports = []
    for case in args.expect:
        pairs = []
        for pair in range(5):
            variants = []
            for variant in ('baseline', 'final'):
                found = [m for m in manifests if (m['case'], m['pair'], m['variant']) == (case, pair, variant)]
                assert len(found) == 1
                m = found[0]
                assert m['valid'] and m['exit_code'] == 0
                rows = list(csv.DictReader(Path(m['output']).open()))
                assert rows and all(r['build_type'] == 'RelWithDebInfo' for r in rows)
                counts = workload(case, m, rows)
                total = sum(int(r['nanoseconds']) for r in rows)
                assert total > 0 and all(v > 0 for v in counts.values())
                integrity = re.findall(r'INTEGRITY,[^\n]+', Path(m['output']).with_suffix('.log').read_text())
                variants.append({'source': rows[0]['git_commit'], 'exe_sha256': m['sha256'], 'counts': counts,
                    'total_ns': total, 'ns_per_operation': {k: total / n for k, n in counts.items()},
                    'first': {k: rows[0][k] for k in WORK if k in rows[0]},
                    'last': {k: rows[-1][k] for k in WORK if k in rows[-1]},
                    'errors': {k: rows[-1].get(k) for k in ('errors', 'failures')}, 'integrity': integrity,
                    'rows': rows})
            a, b = variants
            assert a['counts'] == b['counts'] and len(a['rows']) == len(b['rows'])
            assert all(all(x.get(k) == y.get(k) for k in WORK) for x, y in zip(a['rows'], b['rows']))
            del a['rows'], b['rows']
            pairs.append({'pair': pair, 'baseline': a, 'candidate': b,
                'percent': 100 * (b['total_ns'] / a['total_ns'] - 1), 'delta_ns': b['total_ns'] - a['total_ns']})
        result = {'case': case, 'pairs': pairs, 'median_percent': statistics.median(p['percent'] for p in pairs),
            'range_percent': [min(p['percent'] for p in pairs), max(p['percent'] for p in pairs)],
            'median_baseline_ns': statistics.median(p['baseline']['total_ns'] for p in pairs),
            'median_candidate_ns': statistics.median(p['candidate']['total_ns'] for p in pairs)}
        reports.append(result)
        print(case, round(result['median_percent'], 3), result['range_percent'])
    (root / 'validated-costs.json').write_text(json.dumps({'comparisons': reports,
        'statistical_unit': 'Independent process pair; frames are not independent replicates',
        'unit_scope': 'Every unit cost amortizes the complete batch; it is not an isolated resume/handler cost',
        'error_scope': 'Missing legacy error columns remain null; retained integrity records are not total error counters'}, indent=2))

if __name__ == '__main__': main()
