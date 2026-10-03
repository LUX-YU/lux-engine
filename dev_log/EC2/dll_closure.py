"""Windows import closure for the actual PLAYER script and installed native consumer."""
from pathlib import Path
import hashlib,json,re,subprocess
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
b=Path(c['build']);prefix=Path(c['prefix']);out=w/'dll-proof';out.mkdir(exist_ok=True)
cases=[('player',b.with_name('ec2-final-player')/'bin/scene_script_asset_scene_test.exe',
        [b.with_name('ec2-final-player')/'bin']),
       ('native-sdk',b.with_name('ec2-final-native-sdk')/'native/bin/ec2_consumer.exe',[prefix/'bin'])]
records=[]
for label,exe,roots in cases:
    assert exe.is_file(),str(exe)
    roots=[exe.parent,*roots,Path('D:/Development/vcpkg/installed/x64-windows/bin'),Path('C:/Windows/System32')]
    pending=[exe];seen=set();nodes=[]
    while pending:
        file=pending.pop(0)
        if file.name.lower() in seen:continue
        seen.add(file.name.lower())
        output=subprocess.check_output(['dumpbin','/nologo','/dependents',str(file)],stderr=subprocess.STDOUT)
        log=out/(label+'-'+file.name+'.log');log.write_bytes(output)
        names=re.findall(r'^\s+([A-Za-z0-9_.-]+\.dll)\s*$',output.decode(errors='replace'),re.M|re.I)
        nodes.append(dict(path=str(file),sha256=hashlib.sha256(file.read_bytes()).hexdigest(),imports=names,
                          log=log.relative_to(w).as_posix()))
        for name in names:
            assert 'editor' not in name.lower(),name
            if label=='native-sdk':assert not any(x in name.lower() for x in ('lua','vulkan','llvm','spirv')),name
            if name.lower().startswith(('api-ms-','ext-ms-')):continue
            resolved=next((root/name for root in roots if (root/name).is_file()),None)
            assert resolved,name
            if resolved.parent==Path('C:/Windows/System32'):continue
            pending.append(resolved)
    records.append(dict(case=label,executable=str(exe),nodes=nodes,
        scope='Actual linked import closure; successful test execution independently proves loader resolution. Not a sampled Process module timeline.'))
(w/'dll-closure.json').write_text(json.dumps(records,indent=2)+'\n')
print('Actual PLAYER and installed native executable DLL closure: no Editor; native also no Lua/GPU/LLVM')
