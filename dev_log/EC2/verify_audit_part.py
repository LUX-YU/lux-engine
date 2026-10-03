from pathlib import Path
import hashlib, json, sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
if sys.argv[1]=='installed':
    x=json.loads((w/'installed-audit.json').read_text())
    assert x['implementation_sha']==c['implementation_sha']
    assert not x['retired_residual'] and not x['old_prefix_references']
    assert x['sdk_identities']
    for f in x['sdk_identities']:
        assert hashlib.sha256((Path(c['prefix'])/f['path']).read_bytes()).hexdigest()==f['sha256']
    print('Fresh installed SDK: identity files verified, no retired public names or old-prefix package references')
elif sys.argv[1]=='modules':
    x=json.loads((w/'module-header-sync.json').read_text());assert x
    for f in x:
        current=Path(f['destination']).read_bytes()
        assert hashlib.sha256(current).hexdigest()==f['sha256']
        assert current.replace(b'\r\n',b'\n')==(Path(c['source'])/f['source']).read_bytes().replace(b'\r\n',b'\n')
    print('Exact changed modules public headers synchronized to three prefixes:',len(x),
          'file-prefix pairs; Android build remains NOT_RUN')
else:raise ValueError(sys.argv[1])
