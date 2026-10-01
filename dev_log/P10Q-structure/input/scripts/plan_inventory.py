#!/usr/bin/env python3
"""Read-only Git inventory -> PROPOSED layering file plan.

This script never moves, deletes, stages, fetches or edits repository files.
It writes one new JSON report. It is NOT an architecture verifier or approval.
Python 3.10+; Git must be on PATH. All paths are read from the actual Git tree.
"""
from __future__ import annotations
import argparse
import collections
import json
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any


def git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ['git', '-C', str(repo), *args], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode('utf-8', errors='replace').strip())
    return result.stdout


def load_rules(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('version') != 1 or not isinstance(data.get('rules'), list):
        raise ValueError('Expected path-rules version 1 and a rules array.')
    seen: set[tuple[str, str]] = set()
    for rule in data['rules']:
        if not all(k in rule for k in ('id', 'source', 'match', 'action', 'layer', 'batch')):
            raise ValueError('A rule is missing required fields.')
        source = rule['source']
        if not isinstance(source, str) or not source.startswith('editor/') or '..' in PurePosixPath(source).parts:
            raise ValueError(f'Unsafe source rule: {source!r}')
        if rule['match'] not in ('exact', 'prefix'):
            raise ValueError(f'Unknown match mode: {rule["match"]}')
        key = (source, rule['match'])
        if key in seen:
            raise ValueError(f'Duplicate source rule: {source}')
        seen.add(key)
        dest = rule.get('destination')
        if dest is not None and (not isinstance(dest, str) or not dest.startswith('editor/') or '..' in PurePosixPath(dest).parts):
            raise ValueError(f'Unsafe destination rule: {dest!r}')
    return data


def choose(path: str, rules: list[dict[str, Any]]) -> dict[str, Any] | None:
    matches = [r for r in rules if
               (r['match'] == 'exact' and path == r['source']) or
               (r['match'] == 'prefix' and path.startswith(r['source']))]
    if not matches:
        return None
    return max(matches, key=lambda r: (len(r['source']), r['match'] == 'exact'))


def inventory(repo: Path, ref: str, ruleset: dict[str, Any]) -> dict[str, Any]:
    commit = git(repo, 'rev-parse', '--verify', '--end-of-options', f'{ref}^{{commit}}').decode().strip()
    entries = git(repo, 'ls-tree', '-r', '-z', '--full-tree', commit, '--', 'editor/').split(b'\0')
    rows: list[dict[str, Any]] = []
    hits: collections.Counter[str] = collections.Counter()
    for record in entries:
        if not record:
            continue
        metadata, raw_path = record.split(b'\t', 1)
        mode, kind, blob = metadata.decode('ascii').split(' ', 2)
        # Refuse undecodable names rather than silently omit a tracked file.
        path = raw_path.decode('utf-8', errors='strict')
        rule = choose(path, ruleset['rules'])
        destination = None
        action = 'REVIEW_REQUIRED'
        note = 'No matching source rule. Determine actual owner, target and consumers.'
        if rule:
            hits[rule['id']] += 1
            action, note = rule['action'], rule.get('note', '')
            if rule.get('destination'):
                suffix = path[len(rule['source']):] if rule['match'] == 'prefix' else ''
                destination = rule['destination'] + suffix
        filename = PurePosixPath(path).name
        if filename == 'CMakeLists.txt':
            action = 'REVIEW_BUILD_MERGE'
            note += ' | Read actual targets, sources, install/export and generation dependencies before merging.'
        elif filename.lower().endswith('.md'):
            action = 'REVIEW_DOCUMENT'
            note += ' | Rewrite/merge long-lived responsibilities; never overwrite frozen history.'
        if kind != 'blob' or mode == '120000':
            action = 'REVIEW_NONREGULAR'
            note += ' | Gitlink/symlink/non-regular object: do not materialize or move automatically.'
        rows.append({
            'source': path, 'source_blob': blob, 'git_mode': mode, 'git_kind': kind,
            'rule': rule['id'] if rule else None,
            'proposed_action': action,
            'proposed_layer': rule['layer'] if rule else 'UNCLASSIFIED',
            'proposed_batch': rule['batch'] if rule else 'L0',
            'proposed_destination': destination,
            'actual_target': None, 'logical_include': None, 'consumers': [],
            'symbol_splits': [], 'verification_ids': [],
            'approved': False, 'note': note,
        })
    destinations: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        if row['proposed_destination']:
            destinations[row['proposed_destination'].casefold()].append(row)
    collisions = []
    for folded, colliding in destinations.items():
        if len(colliding) > 1:
            collisions.append({'casefolded_destination': folded,
                               'sources': [r['source'] for r in colliding],
                               'resolution': 'UNRESOLVED_MANUAL_MERGE_OR_SPLIT'})
            for row in colliding:
                row['destination_collision'] = True
    return {
        'status': 'PROPOSAL_REQUIRES_L0_REVIEW',
        'reference_commit_in_rules': ruleset.get('reference_commit'),
        'actual_commit': commit,
        'baseline_changed': commit != ruleset.get('reference_commit'),
        'git_tree_scope': 'editor/',
        'source_content_semantically_reviewed': False,
        'target_dependencies_verified': False,
        'tracked_entries': len(rows),
        'rules_with_no_matches': [r['id'] for r in ruleset['rules'] if not hits[r['id']]],
        'destination_collisions': collisions,
        'files': rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('--rules', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        repo = Path(git(args.repo.resolve(), 'rev-parse', '--show-toplevel').decode().strip()).resolve()
        out = args.out.resolve()
        if out.exists():
            raise ValueError(f'Report already exists; choose a new path: {out}')
        try:
            relative = out.relative_to(repo)
        except ValueError:
            relative = None
        if relative is not None:
            if '.git' in relative.parts:
                raise ValueError('Output must not be inside .git.')
            tracked = git(repo, 'ls-files', '--', relative.as_posix())
            if tracked.strip():
                raise ValueError('Output would replace a tracked repository path.')
        # The actual worktree state is recorded, never repaired.
        before = git(repo, 'status', '--porcelain=v1', '-z')
        report = inventory(repo, args.ref, load_rules(args.rules))
        after = git(repo, 'status', '--porcelain=v1', '-z')
        if before != after:
            raise RuntimeError('Worktree changed during inventory; retry after reviewing concurrent edits.')
        report['worktree_was_clean_before_report'] = not before
        report['repository_mutation_performed'] = False
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        print(f'Wrote PROPOSED plan for {report["tracked_entries"]} tracked entries to {out}')
        print('No semantic/architecture approval. Resolve all actions and destination collisions in L0.')
        return 0
    except (OSError, RuntimeError, ValueError, UnicodeError, json.JSONDecodeError) as error:
        print(f'Inventory failed: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
