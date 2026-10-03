from pathlib import Path
import json,hashlib,subprocess
w=Path(__file__).resolve().parent
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
items=json.loads((w/'module-header-sync.json').read_text())
for item in items:
    actual=Path(item['destination']).read_bytes()
    assert hashlib.sha256(actual).hexdigest()==item['sha256'],item
    expected=subprocess.check_output(['git','show','HEAD:'+item['source']],cwd=s)
    assert actual.replace(b'\r\n',b'\n')==expected.replace(b'\r\n',b'\n'),item
print('PASS: all',len(items),'module header copies match final Git bytes in Debug/RelWithDebInfo/Android includes; no Android build claimed')
