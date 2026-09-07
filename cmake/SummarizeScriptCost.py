"""Validate SR-3 closeout workload counters and report process-paired total and unit costs.

CSV calls have different meanings: C++ is per frame, Lua cumulative script calls,
Flow sync cumulative provider calls (two per script call), Flow event cumulative provider/resume calls.
Missing error columns are unavailable, never silently converted to zero.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics


def work(record, rows):
    case = record['case']
    command = record['command']
    warmups = int(command[command.index('--warmups') + 1])
    size = int(command[command.index('--size') + 1])
    frames = len(rows)
    budget = record['effective_resume_budget']
    for row in rows:
        assert int(row['size']) == size and row['build_type'] == 'RelWithDebInfo'
        assert int(row['active_instances']) == size
    result = dict(script_calls=0, provider_calls=0, resumes=0, occurrences=0,
                  callback_attempts=0, completed_business=0, hook_occurrences=0)
    if case == 'event-fanout':
        assert frames == 6
        for i, row in enumerate(rows):
            assert int(row['calls']) == size and int(row['resumes']) == i * budget
            assert int(row['queue_depth']) == size - i * budget
            assert int(row['events']) == (1 if i == 0 else 0)
        result.update(resumes=size, occurrences=1, completed_business=size)
        result['normalization_note'] = 'One occurrence plus five drains; initial waiter registration is outside this batch.'
    else:
        result['hook_occurrences'] = frames
        result['callback_attempts'] = frames * size
        for i, row in enumerate(rows):
            elapsed_frames = warmups + i + 1
            if case == 'cpp-update':
                assert int(row['calls']) == size and int(row['queue_depth']) == 0
            elif case in ('lua-update', 'lua-event'):
                assert int(row['calls']) == elapsed_frames * size and int(row['queue_depth']) == 0
                if case == 'lua-event':
                    assert int(row['resumes']) == elapsed_frames * size and int(row['events']) == 1
            elif case == 'flow-update':
                assert int(row['calls']) == 2 * elapsed_frames * size
                assert int(row['resumes']) == 0 and int(row['queue_depth']) == 0
            elif case == 'flow-event':
                assert int(row['calls']) == elapsed_frames * budget
                assert int(row['resumes']) == elapsed_frames * budget
                assert int(row['started']) == size + (elapsed_frames - 1) * budget
                assert int(row['queue_depth']) == size - budget
            else:
                raise ValueError(case)
        result['script_calls'] = frames * (budget if case == 'flow-event' else size)
        result['completed_business'] = result['script_calls']
        if case.startswith('flow-'):
            result['provider_calls'] = frames * (budget if case == 'flow-event' else 2 * size)
        if case.endswith('event'):
            result['resumes'] = frames * (budget if case == 'flow-event' else size)
            result['occurrences'] = frames
        result['normalization_note'] = ('Flow event keeps 8,000 queued operations; completed_business is verified '
            'provider-call/resume delta. Its legacy completed column is sequence-only and remains zero.' if
            case == 'flow-event' else 'Cumulative counters exclude all warmup operations; batch includes the whole frame.')
    result['final_backlog'] = {k: int(rows[-1][k]) for k in
        ('queue_depth', 'external_queue_depth', 'continuations', 'awaitables', 'event_waiters') if k in rows[-1]}
    result['errors'] = {k: int(rows[-1][k]) if k in rows[-1] else None for k in ('errors', 'failures')}
    result['error_limit'] = 'Legacy benchmark CSV does not export a ScriptSystem failure count; process exit and exact work counters are checked separately.'
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--root', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    reports = []
    for comparison in ('b0-b1', 'b1-b2', 'b0-b2'):
        directory = Path(args.root) / ('final-' + comparison)
        records = json.loads((directory / 'runs.json').read_text())
        equality = json.loads((directory / 'business-comparisons.json').read_text())
        assert len(records) == 72 and all(r['valid'] for r in records)
        assert len(equality) == 36 and all(r['business_equal'] for r in equality)
        for r in records:
            label = f"{r['case']}-{r['mode']}-{r['pair']}-{r['variant']}"
            rows = list(csv.DictReader((directory / (label + '.csv')).open()))
            if r['mode'] == 'performance':
                r['effective_work'] = work(r, rows)
                r['source_sha'] = rows[0]['git_commit']
                r['ns_per_operation'] = {k: r['total_ns'] / v for k, v in r['effective_work'].items()
                    if k in ('script_calls', 'provider_calls', 'resumes', 'occurrences', 'callback_attempts',
                             'completed_business', 'hook_occurrences') and v > 0}
        for case in sorted({r['case'] for r in records}):
            pairs = []
            for i in range(5):
                a, b = [next(r for r in records if r['case'] == case and r['mode'] == 'performance'
                    and r['pair'] == i and r['variant'] == v) for v in ('baseline', 'candidate')]
                assert a['effective_work'] == b['effective_work']
                pairs.append(dict(pair=i, baseline=a, candidate=b, delta_ns=b['total_ns'] - a['total_ns'],
                    percent=100 * (b['total_ns'] / a['total_ns'] - 1)))
            percents = [r['percent'] for r in pairs]
            reports.append(dict(comparison=comparison, case=case, pairs=pairs,
                median_percent=statistics.median(percents), range_percent=[min(percents), max(percents)],
                median_baseline_ns=statistics.median(r['baseline']['total_ns'] for r in pairs),
                median_candidate_ns=statistics.median(r['candidate']['total_ns'] for r in pairs),
                median_delta_ns=statistics.median(r['delta_ns'] for r in pairs),
                diagnostics=[r for r in records if r['case'] == case and r['mode'] == 'diagnostic']))
    Path(args.output).write_text(json.dumps(dict(statistical_unit='Independent process pair; frames are not replicates',
        allocation_limit='EXE-local new and explicitly enabled Lua VM accounting; not all DLL allocations',
        comparisons=reports), indent=2))
    for r in reports:
        print(r['comparison'], r['case'], round(r['median_percent'], 2), r['range_percent'])


if __name__ == '__main__':
    main()
