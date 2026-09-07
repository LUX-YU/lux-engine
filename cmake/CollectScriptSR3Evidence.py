"""Archive raw SR-3 logs and diagnostic source, with installed identities; never archive compiled binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', action='append', required=True)
    parser.add_argument('--source', action='append', required=True)
    parser.add_argument('--prefix', action='append', required=True)
    parser.add_argument('--consumers', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    identities = {'sources': [], 'prefixes': [], 'installed_private_headers': []}
    for value in args.source:
        path = Path(value)
        identities['sources'].append({'path': str(path), 'commit': git(path, 'rev-parse', 'HEAD'),
                                      'status': git(path, 'status', '--porcelain'),
                                      'origin': git(path, 'remote', 'get-url', 'origin')})
    private = {'ScriptInstances.hpp', 'ScriptBindings.hpp', 'ScriptPreparer.hpp', 'ScriptRuntimeAccess.hpp'}
    for value in args.prefix:
        prefix = Path(value)
        files = []
        for path in sorted(prefix.rglob('*')):
            if not path.is_file():
                continue
            relative = path.relative_to(prefix)
            if 'include' in relative.parts and path.name in private:
                identities['installed_private_headers'].append(str(path))
            if ('bin' in relative.parts and path.suffix.lower() in {'.dll', '.exe'}) or (
                    'include' in relative.parts and 'script' in str(relative).lower()):
                files.append({'path': relative.as_posix(), 'size': path.stat().st_size, 'sha256': digest(path)})
        identities['prefixes'].append({'prefix': str(prefix), 'files': files})
    if identities['installed_private_headers']:
        raise RuntimeError('Private implementation leaked into installed headers')
    (output / 'identity.json').write_text(json.dumps(identities, indent=2), encoding='utf-8')
    closure = {}
    for name, forbidden in {
        'script-runtime-input': ('lux_engine_world', 'lux_engine_scene', 'lux_engine_process'),
        'script-description': ('lux_engine_process', 'lux_engine_scene_script_runtime', 'lux_engine_scene_composition')
    }.items():
        ninja = Path(args.consumers) / name / 'build/build.ninja'
        links = [line.strip() for line in ninja.read_text().splitlines() if 'LINK_LIBRARIES =' in line]
        if not links:
            raise RuntimeError('Missing actual installed link line: ' + str(ninja))
        reachable = {key: any(key in line for line in links) for key in forbidden}
        if any(reachable.values()):
            raise RuntimeError('Forbidden installed closure: ' + name)
        closure[name] = {'link_libraries': links, 'forbidden_reachable': reachable}
    (output / 'installed-link-closure.json').write_text(json.dumps(closure, indent=2))
    entries = {}
    for value in args.root:
        root = Path(value)
        for path in root.rglob('*'):
            if not path.is_file():
                continue
            relative = path.relative_to(root)
            diagnostic = any(part in {'scale', 'probes', 'flow-allocations', 'source'} for part in relative.parts)
            selected = path.suffix.lower() in {'.log', '.csv', '.json', '.xml', '.patch', '.diff', '.py', '.ps1'} or (
                path.name in {'CMakeCache.txt', 'CMakeLists.txt', 'build.ninja', 'rules.ninja'}) or (
                diagnostic and path.suffix.lower() in {'.cpp', '.hpp', '.cmake', '.txt'}) or (
                path.name == 'v1.bin' and path.stat().st_size == 288)
            if selected:
                name = root.name + '/' + relative.as_posix()
                if name in entries:
                    raise RuntimeError('Duplicate archive path: ' + name)
                entries[name] = path
    for name in ('identity.json', 'installed-link-closure.json'):
        entries['identity/' + name] = output / name
    index = [{'path': name, 'source': str(path), 'size': path.stat().st_size, 'sha256': digest(path)}
             for name, path in sorted(entries.items())]
    (output / 'raw-files.json').write_text(json.dumps(index, indent=2), encoding='utf-8')
    archive = output / 'SR3-raw-evidence.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, path in sorted(entries.items()):
            bundle.write(path, name)
    with zipfile.ZipFile(archive) as bundle:
        for entry in index:
            if hashlib.sha256(bundle.read(entry['path'])).hexdigest() != entry['sha256']:
                raise RuntimeError('Archive content mismatch: ' + entry['path'])
    (output / 'SHA256SUMS').write_text(digest(archive) + '  ' + archive.name + '\n')
    print(json.dumps({'files': len(index), 'bytes': archive.stat().st_size, 'sha256': digest(archive)}))


if __name__ == '__main__':
    main()
