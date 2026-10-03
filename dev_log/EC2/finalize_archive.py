from pathlib import Path
import hashlib,json,shutil,subprocess,sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
a=w/'archive';assert a.is_dir()
records=json.loads((w/'portable-results.json').read_text())
assert len(records)==3 and all(r['expected_result'] for r in records)
proof=a/'portable-proof';proof.mkdir(exist_ok=True)
for r in records:
    p=w/'归档 可搬迁验证'/r['log']
    assert hashlib.sha256(p.read_bytes()).hexdigest()==r['sha256']
    shutil.copy2(p,proof/p.name)
shutil.copy2(w/'portable-results.json',a/'portable-results.json')
shutil.copy2(w/'report.md',a/'report.md')
shutil.copy2(Path(__file__),a/Path(__file__).name)
(a/'pending-input.json').write_text(json.dumps(dict(
    source='User reply in current conversation',question='EC2 还剩原生输入验收，需要短暂接管鼠标和键盘，约一分钟。你现在方便留出这段时间吗？我会在开始前提示。',
    answer='稍后再做',status='NOT_RUN_USER_DEFERRED',permanent_waiver=False,
    tests=['editor.desktop_native_input','installed.desktop.native_input'],
    source_build=c['build'],sdk_build=str(Path(c['build']).with_name('ec2-final-sdk')/'desktop-views'),
    implementation_sha=c['implementation_sha']),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
artifacts=[dict(path=p.relative_to(a).as_posix(),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
           for p in sorted(a.rglob('*')) if p.is_file() and p!=a/'artifacts.json']
(a/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
subprocess.run([sys.executable,str(Path(c['source'])/'editor/tests/architecture/validate_ec2_evidence.py'),
    '--source',c['review_source'],'--archive',str(a)],check=True)
destination=Path(c['review_source'])/'dev_log/EC2'
assert not destination.exists(),'Never replace a historical snapshot'
shutil.copytree(a,destination)
subprocess.run([sys.executable,str(Path(c['source'])/'editor/tests/architecture/validate_ec2_evidence.py'),
    '--source',c['review_source'],'--archive',str(destination)],check=True)
print('Record ready for separate commit:',destination,len(artifacts),'hashed files; phase status PARTIAL')
