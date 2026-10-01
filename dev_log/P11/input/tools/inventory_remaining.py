#!/usr/bin/env python3
"""Read-only Git inventory for P11/P12 cleanup. Never deletes or edits repository files.

The report is a candidate list, NOT a proof that all references are active or that a
phase passed. Inspect source semantics, CMake graphs, SDK and actual runtime separately.
Only committed Git objects at --ref are read; user worktree differences are untouched.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

LEGACY_ROOTS = (
    'editor/app/', 'editor/assets/', 'editor/context/', 'editor/launcher/',
    'editor/metadata/', 'editor/plugins/', 'editor/tools/',
    'editor/transition/', 'editor/ui/',
)
FORMAL_ROOTS = ('editing', 'authoring', 'activities', 'workbench', 'application', 'tests')
SYMBOLS = (
    'EditorContext', 'PaneManager', 'SceneEditor', 'MaterialEditor',
    'FlowForgeEditor', 'WorkspaceRequest', 'EWorkspaceAction', 'WorkspaceData',
    'PaneRegistration', 'AssetEditorRegistration', 'CommandRegistration',
    'TAssetSave', 'SceneSaveCapture', 'LegacyPersistenceState', 'EditorTestAccess',
)
OLD_TARGETS = ('editor_context', 'editor_ui', 'editor_app',
               'editor_scene', 'editor_material', 'editor_flowforge', 'editor_metadata')
TEXT_SUFFIXES = {'.hpp', '.h', '.cpp', '.cc', '.cxx', '.cmake', '.py', '.ps1', '.json'}
PATTERN = re.compile(r'\b(?:' + '|'.join(map(re.escape, SYMBOLS + OLD_TARGETS)) + r')\b')


def git(repo: Path, *args: str, data: bytes | None = None) -> bytes:
    completed = subprocess.run(
        ['git', '-C', str(repo), *args], input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode('utf-8', 'replace').strip()
        raise RuntimeError(f'git {args[0]} failed: {detail}')
    return completed.stdout


def collect(repo: Path, ref: str) -> dict[str, Any]:
    if not ref or ref.startswith('-') or '\n' in ref or '\r' in ref:
        raise ValueError('ref must be a commit/ref name, not an option or multiline value')
    oid = git(repo, 'rev-parse', '--verify', f'{ref}^{{commit}}').decode().strip()
    raw = git(repo, 'ls-tree', '-r', '-z', '--full-tree', oid,
              '--', 'editor', 'cmake', 'CMakeLists.txt')
    files: list[dict[str, str]] = []
    roots: set[str] = set()
    for record in raw.split(b'\0'):
        if not record:
            continue
        header, name = record.split(b'\t', 1)
        mode, kind, blob = header.decode('ascii').split()
        path = name.decode('utf-8', 'surrogateescape')
        files.append({'mode': mode, 'kind': kind, 'blob': blob, 'path': path})
        parts = path.split('/')
        if len(parts) >= 3 and parts[0] == 'editor':
            roots.add(parts[1])
    old_files = [f for f in files if f['path'].startswith(LEGACY_ROOTS)]
    selected = [f for f in files if f['kind'] == 'blob' and f['mode'] != '120000'
                and (Path(f['path']).suffix in TEXT_SUFFIXES or
                     Path(f['path']).name == 'CMakeLists.txt')]
    blob_ids = list(dict.fromkeys(f['blob'] for f in selected))
    contents: dict[str, bytes] = {}
    if blob_ids:
        batch = git(repo, 'cat-file', '--batch', data=('\n'.join(blob_ids) + '\n').encode('ascii'))
        offset = 0
        for expected in blob_ids:
            end = batch.index(b'\n', offset)
            got, kind, size_text = batch[offset:end].decode('ascii').split()
            if got != expected or kind != 'blob':
                raise RuntimeError('Unexpected Git batch record; report was not generated')
            size = int(size_text)
            start = end + 1
            stop = start + size
            if stop >= len(batch) or batch[stop:stop + 1] != b'\n':
                raise RuntimeError('Truncated Git batch output')
            contents[expected] = batch[start:stop]
            offset = stop + 1
    hits: list[dict[str, Any]] = []
    for f in selected:
        try:
            text = contents[f['blob']].decode('utf-8-sig')
        except UnicodeError:
            hits.append({'path': f['path'], 'blob': f['blob'], 'needs_review': 'non_utf8_text'})
            continue
        for number, line in enumerate(text.splitlines(), 1):
            matches = sorted(set(PATTERN.findall(line)))
            if matches:
                hits.append({'path': f['path'], 'blob': f['blob'], 'line': number,
                             'symbols': matches,
                             'classification': 'UNREVIEWED: may be active code, rule, or negative test'})
    return {
        'schema_version': 1,
        'purpose': 'read_only_inventory_not_acceptance',
        'source_commit': oid,
        'worktree_used_as_source': False,
        'expected_editor_roots': list(FORMAL_ROOTS),
        'actual_editor_roots': sorted(roots),
        'extra_editor_roots': sorted(roots.difference(FORMAL_ROOTS)),
        'legacy_root_files': old_files,
        'symbol_candidates': hits,
        'review_notes': [
            'No source, worktree, index, branch, installed file, or history was changed.',
            'No automatic deletion. Do not remove legitimate editor_assets/editor_storage/editor_editing_scene.',
            'Keyword matches are not an AST/callgraph/build/runtime proof.',
            'Root()/parent constructors, renamed equivalents and plugin ABI require separate semantic review.',
            'History outside editor/cmake was intentionally not scanned or rewritten.',
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('--output', type=Path,
                        help='Optional report path outside the repository; stdout when omitted')
    args = parser.parse_args()
    try:
        top = Path(git(args.repo, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
        if args.output is not None:
            destination = args.output.resolve()
            if destination == top or top in destination.parents:
                raise ValueError('Write report outside the repository, then copy a reviewed report to the ledger')
            if destination.exists():
                raise ValueError('Output already exists; choose a new path to preserve prior reports')
        report = collect(top, args.ref)
        encoded = json.dumps(report, ensure_ascii=True, indent=2) + '\n'
        if args.output is None:
            sys.stdout.write(encoded)
        else:
            # Exclusive creation never overwrites user files; no automatic parent-directory creation.
            with args.output.open('x', encoding='utf-8', newline='\n') as stream:
                stream.write(encoded)
        return 0  # Successful inventory, never a phase PASS verdict.
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'Inventory failed: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
