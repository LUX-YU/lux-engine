from pathlib import Path
import subprocess
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11');fmt=r'D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/clang-format.exe'
changed=subprocess.check_output(['git','diff','--name-only','HEAD'],cwd=s,text=True).splitlines()+subprocess.check_output(['git','ls-files','--others','--exclude-standard'],cwd=s,text=True).splitlines()
files=[str(s/f) for f in sorted(set(changed)) if f.endswith(('.cpp','.hpp','.h')) and (s/f).is_file()]
for i in range(0,len(files),25):subprocess.run([fmt,'-i','-style=file:E:/SyncForder/CodeRepos/lux-engine/.clang-format',*files[i:i+25]],check=True)
print('Formatted',len(files),'changed C++ files with project style')
