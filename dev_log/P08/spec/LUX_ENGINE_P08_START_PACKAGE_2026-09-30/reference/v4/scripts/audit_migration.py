#!/usr/bin/env python3
"""Read-only migration seed checks; NOT a C++ AST/build/lifetime verifier.

Exit 0: no violations of the listed static rules were found.
Exit 1: one or more rule violations/candidates need resolution.
Exit 2: input/configuration/inventory error; never reinterpret as pass.
No source file is changed. A report is written only when --output is given.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError(f'Expected JSON object: {path}')
    return value


def stage_number(value: str) -> int:
    if not re.fullmatch(r'P(?:0[0-9]|1[0-3])', value):
        raise ValueError(f'Unknown stage {value!r}; expected P00..P13')
    return int(value[1:])


def repository_files(repo: Path) -> list[str]:
    command = ['git', '-C', str(repo), 'ls-files', '-c', '-o', '--exclude-standard', '-z']
    result = subprocess.run(command, check=True, capture_output=True)
    return sorted(set(x for x in result.stdout.decode('utf-8', 'strict').split('\0') if x))


def safe_path(repo: Path, relative: str) -> Path:
    p = Path(relative)
    if p.is_absolute() or '..' in p.parts:
        raise ValueError(f'Unsafe manifest/repository path: {relative!r}')
    full = repo / p
    # Do not follow links to unrelated files or recursively inspect a user's other project.
    if full.is_symlink():
        raise ValueError(f'Symlink in inspected path needs manual inventory: {relative}')
    if not full.resolve().is_relative_to(repo):
        raise ValueError(f'Path escapes repository: {relative}')
    return full


def inspect(repo: Path, stage: str, rules: dict[str, Any]) -> dict[str, Any]:
    stage_i = stage_number(stage)
    paths = repository_files(repo)
    findings: list[dict[str, Any]] = []
    inspected = 0

    def report(rule: str, path: str, detail: str, line: int | None = None) -> None:
        findings.append({'rule': rule, 'path': path, 'line': line, 'detail': detail})

    for item in rules.get('expired_paths', []):
        if stage_i >= stage_number(item['deadline']):
            p = safe_path(repo, item['path'])
            if p.exists():
                report(item['id'], item['path'], f"Old path must be absent by {item['deadline']}; moving it out of target_sources is insufficient.")

    compiled_rules = [(x, re.compile(x['pattern'])) for x in rules.get('forbidden_definitions', [])
                      if stage_i >= stage_number(x['deadline'])]
    source_extensions = set(rules.get('source_extensions', []))
    transition_root = rules['transition_root']
    transition_deadline = stage_number(rules['transition_delete_by'])
    allowed_transition = set(rules.get('transition_allowlist', []))
    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
    forbidden_headers = set(rules.get('new_scope_forbidden_include', []))
    new_scopes = tuple(rules.get('new_scopes', []))
    for rel in paths:
        p = safe_path(repo, rel)
        if not p.exists() or not p.is_file():
            continue  # staged deletion is allowed; skip a submodule directory
        if rel.startswith(transition_root):
            if stage_i >= transition_deadline:
                report('TRANSITION_EXPIRED', rel, 'P12 requires the complete transition source to be absent.')
            elif rel not in allowed_transition:
                report('TRANSITION_UNREGISTERED', rel, 'Temporary source is not in the exact allowlist; register scope and expiry before use.')
        if p.suffix not in source_extensions and p.name != 'CMakeLists.txt':
            continue
        # Test sources are inspected too; comments may produce candidates. Resolve narrowly,
        # not by globally excluding test or compatibility directories.
        text = p.read_text(encoding='utf-8-sig', errors='strict')
        inspected += 1
        for item, pattern in compiled_rules:
            for match in pattern.finditer(text):
                line = text.count('\n', 0, match.start()) + 1
                report(item['id'], rel, item['description'], line)
        if rel.startswith(new_scopes):
            for match in include_re.finditer(text):
                header = match.group(1)
                if header in forbidden_headers:
                    report('NEW_DEPENDS_ON_OLD', rel, f'New module includes obsolete framework: {header}',
                           text.count('\n', 0, match.start()) + 1)
    return {
        'schema': 1, 'stage': stage, 'repo': str(repo),
        'status': 'VIOLATIONS_OR_CANDIDATES' if findings else 'NO_LISTED_STATIC_VIOLATIONS',
        'inspected_source_files': inspected, 'findings': findings,
        'limitations': [
            'Not an AST, build-graph, runtime, ownership, API-completeness or performance proof.',
            'The bundled inventory is a manual seed; use the P00 completed ledger as well.',
            'Comments can match definition expressions; quoted include patterns are literal only.',
            'Tracked and non-ignored untracked sources are inspected; expired exact paths are checked regardless of gitignore.',
            'A renamed God object, macro-generated alias or copied algorithm may require semantic review.'
        ]
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--stage', required=True)
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'manifests' / 'audit-rules.json')
    parser.add_argument('--output', type=Path, help='Optional JSON evidence output; source tree is never edited.')
    args = parser.parse_args(argv)
    try:
        repo = args.repo.resolve(strict=True)
        if not repo.is_dir():
            raise ValueError('Repository must be a directory')
        stage_number(args.stage)
        rules = load_json(args.manifest)
        result = inspect(repo, args.stage, rules)
        payload = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
        if args.output:
            output = args.output.resolve()
            # Do not overwrite a source/manifest file by a mistaken output argument.
            if output == args.manifest.resolve() or output == Path(__file__).resolve() or output.suffix != '.json':
                raise ValueError('--output must name a separate .json evidence file')
            if output.exists():
                raise ValueError('Refusing to overwrite existing evidence; choose a new --output path')
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(payload, encoding='utf-8')
        print(payload, end='')
        return 1 if result['findings'] else 0
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(json.dumps({'status': 'AUDIT_ERROR', 'error': str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
