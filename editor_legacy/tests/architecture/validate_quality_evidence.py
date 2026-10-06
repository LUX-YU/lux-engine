"""P10Q archive gate. Resolve evidence relative to the archive, never recorded producer paths."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, subprocess

STAGES = ['P00','P01','P02','P03','P04','P05','P06','P07','P08','P09','P10','P10Q','P11','P12','P13']
GROUPS = ['sessions','editor-d2','external-feature','scene-ui','views-ui','scene-model','material-model',
          'flowforge-model','persistence','scene-execution','projection-compilation','interaction-views','workspace','desktop-views']

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def archived(directory, name, digest):
    relative = PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts or ':' in name or '\\' in name:
        raise ValueError('Evidence must have an archive-relative path: '+name)
    path = directory.joinpath(*relative.parts).resolve()
    if not path.is_relative_to(directory.resolve()):
        raise ValueError('Evidence escapes archive: '+name)
    data = path.read_bytes()  # Missing evidence is an error, including on another machine.
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('Evidence digest mismatch: '+name)
    return data

def check(source, directory):
    receipt = read(directory/'receipt.json')
    assert receipt['phase'] == receipt['migration_stage'] == receipt['stop_after'] == 'P10Q'
    assert receipt['continuation_authorized'] is False
    def git(*args):
        return subprocess.check_output(['git',*args],cwd=source)
    impl = receipt['implementation_sha']
    base = receipt['input_sha']
    assert base == 'b583e7ffe20e7a1ac55c7119d6a13ac337ebb323'
    git('merge-base','--is-ancestor',base,impl)
    git('merge-base','--is-ancestor',impl,'HEAD')
    rules = json.loads(git('show',impl+':editor_legacy/tests/architecture/rules.json'))
    assert rules['stages'] == STAGES
    files = read(directory/'files.json')
    changed = set(git('diff','--name-only',base,impl).decode().splitlines())
    assert {item['path'] for item in files} == changed
    assert 'editor_legacy/project/src/ProjectBuilder.cpp' not in changed
    for item in files:
        if item['deleted']:
            assert subprocess.run(['git','cat-file','-e',impl+':'+item['path']],cwd=source,
                                  stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode
        else:
            assert hashlib.sha256(git('show',impl+':'+item['path'])).hexdigest()==item['sha256']
    artifacts = read(directory/'artifacts.json')
    assert artifacts
    names = set()
    for item in artifacts:
        assert item['path'] not in names
        names.add(item['path'])
        archived(directory,item['path'],item['sha256'])
    commands = {item['name']:item for item in receipt['commands']}
    assert len(commands) == len(receipt['commands'])
    for command in commands.values():
        assert command['implementation_sha'] == impl
        assert command['log'] in names
        archived(directory,command['log'],command['sha256'])
    # Every qualification has an explicit outcome. NOT_RUN/FAIL cannot manufacture a PASS.
    required = {'windows','linux','clang-cl','cold-build','cpu-native','player','native-input','new-dual-viewport',
                'gpu-ui','editor-scene-pane','operation-negatives','dependency-negatives','performance','sdk'}
    scopes = receipt['qualification']
    assert set(scopes) == required
    for name, result in scopes.items():
        assert result['status'] in ['PASS','FAIL','NOT_RUN','PARTIAL','BLOCKED']
        assert result['commands'] or result['reason']
        for command in result['commands']: assert command in commands
        if result['status'] == 'PASS':
            assert result['commands']
            assert all(commands[c]['exit_code'] == 0 for c in result['commands'])
    complete = all(value['status']=='PASS' for value in scopes.values())
    assert receipt['status'] == ('PASS' if complete else 'PARTIAL')
    assert receipt['input_scope']['system_ime'] == 'NOT_RUN'
    assert {x['id'] for x in receipt['coverage']} == {f'XQ{i:02}' for i in range(1,35)}
    for item in receipt['coverage']:
        assert item['status'] in ['PASS','PARTIAL','NOT_RUN','FAIL']
        assert item['evidence'] or item['reason']
        for name in item['evidence']: assert name in names
    if scopes['windows']['status']=='PASS':
        for name in ['configure','build','no-work','ctest','test-names']:
            assert name in commands and commands[name]['exit_code']==0
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P10Q' in commands['configure']['argv']
        for name in ['build','no-work']:
            args=commands[name]['argv']
            assert args[args.index('--target')+1]=='all' and args[args.index('-j')+1]=='4' and args[-2:]==['-k','0']
        assert b'ninja: no work to do' in archived(directory,commands['no-work']['log'],commands['no-work']['sha256'])
        current=json.loads(archived(directory,commands['test-names']['log'],commands['test-names']['sha256']))
        previous=json.loads(git('show',base+':dev_log/P10-R1/logs/test-names.log'))
        assert {x['name'] for x in previous['tests']} <= {x['name'] for x in current['tests']}
        assert len(previous['tests'])==195
    if scopes['sdk']['status']=='PASS':
        assert receipt['consumer_groups']==GROUPS
        for group in GROUPS:
            for suffix in ['configure','build','no-work','ctest']:
                assert commands[group+'-'+suffix]['exit_code']==0
    for defect in ['C01','C03','C04']:
        assert receipt['known_failures'][defect]['status']=='FAIL'
        assert receipt['known_failures'][defect]['owner']
    return receipt['status']

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--archive',type=Path,required=True)
    args=parser.parse_args()
    print('P10Q archive verified; qualification status:',check(args.source.resolve(),args.archive.resolve()))
