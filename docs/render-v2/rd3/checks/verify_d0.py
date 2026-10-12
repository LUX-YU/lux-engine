"""Read-only RD3-D0 document gate. No build, GPU execution, repair or Git mutation.

Writes only to --output outside source. Mechanical PASS does not close the
explicit semantic findings recorded in D0_ARCHITECTURE_REVIEW.md.
"""
import argparse
import csv
import hashlib
import io
import json
import pathlib
import re
import subprocess
import sys
import urllib.parse
import zipfile

BASE = '246fe21eb6d060801b9a16ddcadde2be89dbbecc'
F6I = '7406965112f09807b9f70aa3e739ff43539969c8'
LEGACY = 'a669409a289a6fa4092f21176397795b1cdb7f3e'
ZIP_SHA = '3ac411f5c6d000b1a7b7a3ffd0e8e3e0b0aeb45936a91433065f9cbc7ead88af'
COMBINED_SHA = '401aa86b5ba881548c993f0ea8c23311e91227b24d4402cf26a756fb66abf459'
PREFIX = 'docs/render-v2/rd3/'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=pathlib.Path, required=True)
    ap.add_argument('--zip', type=pathlib.Path, required=True)
    ap.add_argument('--combined', type=pathlib.Path, required=True)
    ap.add_argument('--user-repo', type=pathlib.Path, required=True)
    ap.add_argument('--user-before', type=pathlib.Path, required=True)
    ap.add_argument('--user-patch', type=pathlib.Path, required=True)
    ap.add_argument('--output', type=pathlib.Path, required=True)
    ap.add_argument('--implementation')
    ap.add_argument('--require-clean', action='store_true')
    a = ap.parse_args()
    root = a.source.resolve()
    d = root / PREFIX
    out = a.output.resolve()
    if out.is_relative_to(root):
        raise SystemExit('Evidence output must be outside source')
    out.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, ok, detail=None):
        checks.append(dict(name=name, passed=bool(ok), detail=detail))

    def git(*args, repo=root):
        return subprocess.check_output(['git', '-C', str(repo), *args])

    def textgit(*args, repo=root):
        return git(*args, repo=repo).decode('utf-8').strip()

    def tree(ref):
        result = {}
        for row in git('ls-tree', '-rlz', ref).split(b'\0'):
            if row:
                meta, path = row.split(b'\t')
                mode, kind, blob, size = meta.split()
                result[path.decode()] = dict(mode=mode.decode(), blob=blob.decode(), bytes=int(size))
        return result

    def rows(path, key, expected):
        content = path.read_text(encoding='utf-8-sig')
        reader = csv.DictReader(io.StringIO(content, newline=''))
        result = list(reader)
        check('CSV shape ' + path.name, bool(result) and all(None not in r and None not in r.values() for r in result))
        ids = [r[key] for r in result]
        check('CSV unique keys ' + path.name, len(ids) == len(set(ids)))
        if expected is not None:
            check('CSV key set ' + path.name, set(ids) == set(expected), len(result))
        return result

    def manifest(path):
        entries = {}
        for line in path.read_text(encoding='utf-8').splitlines():
            digest, rel = line.split('  ', 1)
            check('Manifest unique path ' + rel, rel not in entries)
            safe = not pathlib.PurePosixPath(rel).is_absolute() and '..' not in pathlib.PurePosixPath(rel).parts
            check('Manifest safe path ' + rel, safe)
            entries[rel] = digest
            check('Manifest SHA ' + rel, safe and (d/rel).is_file() and sha((d/rel).read_bytes()) == digest)
        return entries

    head = textgit('rev-parse', 'HEAD')
    check('F6b real V parent is I', textgit('rev-parse', BASE+'^') == F6I)
    check('Source implementation identity', not a.implementation or head == a.implementation, head)
    clean = not git('status', '--porcelain=v1', '--untracked-files=all').strip()
    check('Clean source when required', not a.require_clean or clean, clean)
    check('ZIP exact supplied bytes', sha(a.zip.read_bytes()) == ZIP_SHA)
    check('Separate combined exact supplied bytes', sha(a.combined.read_bytes()) == COMBINED_SHA)
    with zipfile.ZipFile(a.zip) as z:
        check('ZIP CRC', z.testzip() is None)
        names = [n for n in z.namelist() if not n.endswith('/')]
        check('ZIP 38 entries', len(names) == 38 and len(set(names)) == 38)
        members = {}
        for n in names:
            p = pathlib.PurePosixPath(n)
            safe = not p.is_absolute() and '..' not in p.parts and len(p.parts) > 1
            check('ZIP safe path '+n, safe)
            if not safe:
                continue
            rel = pathlib.PurePosixPath(*p.parts[1:]).as_posix()
            members[rel] = z.read(n)
            check('ZIP import byte equality '+rel, (d/rel).is_file() and (d/rel).read_bytes() == members[rel])
        original_manifest = manifest(d/'MANIFEST.sha256')
        check('Input manifest complete coverage', set(original_manifest) == set(members)-{'MANIFEST.sha256'})
    check('Combined attachments byte equality', a.combined.read_bytes() == (d/'FINAL_RD3_全章合订版.md').read_bytes())
    chapters = {p.name[:2]: p for p in d.glob('[0-2][0-9]_*.md')}
    check('21 chapters 00-20', set(chapters) == {f'{i:02}' for i in range(21)})
    combined = (d/'FINAL_RD3_全章合订版.md').read_text(encoding='utf-8')
    for p in chapters.values():
        check('Combined contains exact chapter '+p.name, p.read_text(encoding='utf-8').strip() in combined)
    orders = list((d/'work_orders').glob('*.md'))
    check('Seven work orders', len(orders) == 7 and {p.name[:2] for p in orders} == {f'{i:02}' for i in range(1,8)})
    capids = [f'{p}{i:02}' for p,n in [('F',36),('I',44),('H',6)] for i in range(1,n+1)]
    wids = [f'W{i:02}' for i in range(1,26)]
    rids = [f'REQ{i:02}' for i in range(1,52)]
    cap = rows(d/'appendix/CapabilityMatrix.csv', 'Id', capids)
    workloads = rows(d/'appendix/GoldenWorkloads.csv', 'WorkloadId', wids)
    reqs = rows(d/'appendix/RequirementsTrace.csv', 'RequirementId', rids)
    types = rows(d/'appendix/TypeOwnership.csv', 'TypeOrFunction', None)
    check('60 candidate types/functions', len(types) == 60)
    specials = {'REQ08':'Existing R2-FIX CTest','REQ40':'All Workloads','REQ41':'F0 doc integration','REQ50':'F0 evidence snapshot'}
    for r in reqs:
        refs = r['RequiredGoldenIds'].split(',')
        check('Golden foreign key '+r['RequirementId'], all(x in wids for x in refs) or specials.get(r['RequirementId']) == r['RequiredGoldenIds'])
        check('Chapter foreign key '+r['RequirementId'], all(c in chapters for c in r['RD3OwnerChapters'].split('+')))
    oldroot = root/'docs/render-v2/rd2/appendix'
    mappings = [(cap,'LegacyCapabilityMatrix.csv','Id',{'LegacyCapability':'LegacyCapability','LegacySourceFile':'SourceFile','LegacyOperationSchema':'OperationSchema','RequiredAcceptance':'RequiredAcceptance','HistoricalSourceSHA':'SourceSHA','RequiredEvidence':'EvidenceRequired'}),
                (workloads,'GoldenWorkloads.csv','WorkloadId',{'Name':'Name','HistoricalOracle':'Oracle'}),
                (reqs,'RequirementsTrace.csv','RequirementId',{'HistoricalRequirement':'HardRequirement','RequiredGoldenIds':'EvidenceWorkloads'})]
    for current, filename, key, columns in mappings:
        with (oldroot/filename).open(encoding='utf-8-sig',newline='') as f:
            old = {r[key]:r for r in csv.DictReader(f)}
        check('RD2 full ID preservation '+filename, set(old) == {r[key] for r in current})
        for r in current:
            for newcol, oldcol in columns.items():
                check('Original contract '+r[key]+' '+newcol, r[newcol] == old[r[key]][oldcol])
    base_tree, current_tree, frozen = tree(BASE), tree(head), tree(LEGACY)
    check('All existing tracked entries unchanged', all(current_tree.get(p) == v for p,v in base_tree.items()), len(base_tree))
    additions = sorted(set(current_tree)-set(base_tree))
    check('Only D0 add-only scope', all(p.startswith(PREFIX) for p in additions), additions)
    if a.require_clean:
        # Compare actual working bytes with Git objects, not just index/stat caches.
        dirty_bytes=[]
        for path,v in current_tree.items():
            p=root/path
            data=p.read_bytes() if p.is_file() else b''
            blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
            if not p.is_file() or blob != v['blob']:
                dirty_bytes.append(path)
        check('All working source bytes equal locked Git blobs', not dirty_bytes, dirty_bytes)
    legacy_manifest=json.loads((root/'docs/render-v2/LEGACY_SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    files=legacy_manifest['files']
    check('Legacy frozen source identity/count', legacy_manifest['source_sha']==LEGACY and len(files)==719)
    for f in files:
        expected={k:f[k] for k in ('mode','blob','bytes')}
        check('Legacy frozen original '+f['source'], frozen.get(f['source'])==expected)
        check('Legacy protected copy '+f['destination'], current_tree.get(f['destination'])==expected)
    for r in cap:
        for field in ('LegacySourceFile','LegacyOperationSchema'):
            if '/' in r[field]: check('Capability source FK '+r['Id']+' '+field, r[field] in base_tree)
    anchors=json.loads((d/'D0_SOURCE_ANCHORS.json').read_text(encoding='utf-8'))
    check('Source anchor identity', anchors['source_commit']==BASE)
    for r in anchors['anchors']:
        data=git('show', BASE+':'+r['path'])
        check('Source anchor '+r['path'], base_tree[r['path']]['blob']==r['blob'] and len(data)==r['bytes'] and sha(data)==r['sha256'])
    migration=rows(d/'D0_MIGRATION_MAP.csv','Id',[f'M{i:02}' for i in range(1,31)])
    for r in migration:
        data=git('show',BASE+':'+r['SourcePath']).decode('utf-8-sig').splitlines()
        check('Migration source symbol '+r['Id'], r['SourceSymbol'] in data[int(r['SourceLine'])-1] and base_tree[r['SourcePath']]['blob']==r['SourceBlob'])
    trace=rows(d/'D0_TRACE_REVIEW.csv','Id',capids+wids+rids)
    for r in trace:
        check('Trace owner/stage/oracle '+r['Id'], all(r[k].strip() for k in ['Owner','InputStage','ProposedStages','Acceptance','Disposition']))
        check('Trace Golden FK '+r['Id'], all(w in wids for w in r['GoldenIds'].split(',') if w))
        check('Trace work order FK '+r['Id'], r['WorkOrders']=='ALL_WORK_ORDERS' or all((d/w).is_file() for w in r['WorkOrders'].split(';')))
    type_review=rows(d/'D0_TYPE_REVIEW.csv','TypeOrFunction',[r['TypeOrFunction'] for r in types])
    for r in type_review:
        check('Type review responsibility '+r['TypeOrFunction'], all(r[k].strip() for k in ['UniqueOwner','MutationAuthority','LifetimeRule','FirstConsumer','WhyThisForm','FailureAndRollback','ProposedStages','RequiredProof']))
    external_links=[]
    for p in d.rglob('*.md'):
        content=p.read_text(encoding='utf-8')
        prose=[]
        fence=None
        for line in content.splitlines():
            marker=re.match(r'^\s{0,3}(`{3,}|~{3,})',line)
            if marker:
                run=marker.group(1)
                if fence is None:
                    fence=run
                elif run[0]==fence[0] and len(run)>=len(fence):
                    fence=None
                continue
            if fence is None:
                prose.append(line)
        check('Markdown balanced fences '+p.name,fence is None)
        rendered=re.sub(r'`+[^`\n]*`+','', '\n'.join(prose))
        for target in re.findall(r'\[[^\]\n]+\]\(([^\n)]+)\)', rendered):
            target=urllib.parse.unquote(target.strip('<>'))
            if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:',target):
                external_links.append(target);continue
            local=target.split('#',1)[0]
            if local:check('Markdown local link '+p.name+' -> '+target, (p.parent/local).exists())
    d0manifest=manifest(d/'D0_MANIFEST.sha256')
    actual={p.relative_to(d).as_posix() for p in d.rglob('*') if p.is_file()}
    check('D0 manifest exact coverage', set(d0manifest)==actual-{'D0_MANIFEST.sha256','D0_VERIFICATION.md'})
    before=json.loads(a.user_before.read_text(encoding='utf-8-sig'))
    check('User HEAD unchanged',textgit('rev-parse','HEAD',repo=a.user_repo)==before['head'])
    check('User status unchanged',git('status','--porcelain=v1','--untracked-files=all',repo=a.user_repo).decode().replace('\r\n','\n')==before['status'].replace('\r\n','\n'))
    check('User complete binary diff unchanged',git('diff','--binary','HEAD',repo=a.user_repo)==a.user_patch.read_bytes())
    for path,digest in before['files'].items():
        check('User local bytes '+path,sha((a.user_repo/path).read_bytes())==digest)
    gitdir=pathlib.Path(textgit('rev-parse','--absolute-git-dir'))
    common=pathlib.Path(textgit('rev-parse','--git-common-dir'))
    if not common.is_absolute(): common=(root/common).resolve()
    objectfiles=[p for p in (common/'objects').rglob('*') if p.is_file()]
    isolation={'git_dir':str(gitdir),'common_dir':str(common),'alternates':(common/'objects/info/alternates').exists(),'max_object_links':max((p.stat().st_nlink for p in objectfiles),default=0),'object_files':len(objectfiles),'clean':clean}
    if a.require_clean:
        check('Independent clone no alternates/hardlinks',not isolation['alternates'] and isolation['max_object_links']==1 and gitdir==common)
    failures=[c for c in checks if not c['passed']]
    result={'head':head,'base':BASE,'implementation':a.implementation,'mechanical_status':'PASS' if not failures else 'FAIL',
            'design_freeze_status':'PARTIAL','open_findings':['R01','R02','R03','R04'],'clarification':['R05'],
            'checks':len(checks),'failed':failures,'isolation':isolation,'counts':{'chapters':len(chapters),'work_orders':len(orders),'capabilities':len(cap),'workloads':len(workloads),'requirements':len(reqs),'types':len(types),'trace_rows':len(trace),'migration_rows':len(migration),'source_anchors':len(anchors['anchors']),'legacy':len(files)},
            'manifest_sha256':sha((d/'D0_MANIFEST.sha256').read_bytes()),'input_manifest_sha256':sha((d/'MANIFEST.sha256').read_bytes()),'external_web_links_not_checked':sorted(set(external_links)),'added_paths':additions}
    (out/'checks.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return 1 if failures else 0


if __name__=='__main__':
    sys.exit(main())
