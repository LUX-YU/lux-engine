"""Loader diagnostic only, not a replacement for the failed integration test."""
from pathlib import Path
import ctypes, json, os, subprocess

w = Path(__file__).resolve().parent
c = json.loads((w / 'final-config.json').read_text())
b = Path(c['build'])
catalog = json.loads((b / 'share/lux-engine/plugins/catalog.json').read_text())
selected = next(p['plugin'] for p in catalog['plugins'] if p['plugin']['id'] == 'lux.builtin.scene_render')
print('Actual selected plugin record:', json.dumps(selected, indent=2), flush=True)
library = b / selected['runtime_library']['path']
print(subprocess.check_output(['dumpbin', '/dependents', str(library)], text=True), flush=True)
directories = [os.add_dll_directory(str(path)) for path in
               [b / 'bin', Path('D:/Development/vcpkg/installed/x64-windows/bin')]]
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
load = kernel.LoadLibraryExW
load.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
load.restype = ctypes.c_void_p
handle = load(str(library), None, 0x1100)
error = ctypes.get_last_error()
print('Actual LoadLibraryExW handle:', handle, 'GetLastError:', error, flush=True)
assert not handle and error == 126
print(subprocess.check_output(['dumpbin', '/dependents', str(b/'bin/lux_engine_scene_render.dll')], text=True))
