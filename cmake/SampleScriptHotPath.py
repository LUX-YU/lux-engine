"""Windows user-mode RIP sampling fallback; diagnostic only, never performance evidence.

Suspends only the benchmark's first thread for GetThreadContext, always resumes it in finally.
Samples include startup/shutdown and suspension overhead; no stacks or hardware counters.
Uses qualified binaries and llvm-symbolizer with their matching PDBs.
"""
import argparse
import collections
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


class ThreadEntry(c.Structure):
    _fields_ = [(n, w.DWORD) for n in ('size', 'usage', 'tid', 'pid', 'priority', 'delta', 'flags')]


class ModuleInfo(c.Structure):
    _fields_ = [('base', c.c_void_p), ('size', w.DWORD), ('entry', c.c_void_p)]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bin', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--flow', action='store_true')
    args = p.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    binary = Path(args.bin)
    exe = binary / ('flowforge_script_runtime_benchmark.exe' if args.flow else 'script_runtime_benchmark.exe')
    cmd = [str(exe), '--group', 'scene-flowforge-update-heavy' if args.flow else 'scene-cpp-update-heavy',
           '--mode', 'performance', '--size', '10000', '--warmups', '60', '--frames', '30000',
           '--seed', '1592598566', '--resume-budget', '2000', '--output', str(root / 'work.csv')]
    if not args.flow:
        cmd += ['--workers', '0']
    env = dict(os.environ)
    env['PATH'] = str(binary) + ';D:/Development/vcpkg/installed/x64-windows/bin;' + env['PATH']
    k = c.WinDLL('kernel32', use_last_error=True)
    ps = c.WinDLL('psapi', use_last_error=True)
    signatures = [(k.CreateToolhelp32Snapshot, [w.DWORD, w.DWORD], w.HANDLE),
                  (k.Thread32First, [w.HANDLE, c.POINTER(ThreadEntry)], w.BOOL),
                  (k.Thread32Next, [w.HANDLE, c.POINTER(ThreadEntry)], w.BOOL),
                  (k.OpenThread, [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
                  (k.SuspendThread, [w.HANDLE], w.DWORD), (k.ResumeThread, [w.HANDLE], w.DWORD),
                  (k.GetThreadContext, [w.HANDLE, c.c_void_p], w.BOOL),
                  (k.CloseHandle, [w.HANDLE], w.BOOL),
                  (ps.EnumProcessModules, [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)], w.BOOL),
                  (ps.GetModuleInformation, [w.HANDLE, w.HMODULE, c.POINTER(ModuleInfo), w.DWORD], w.BOOL),
                  (ps.GetModuleFileNameExW, [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD], w.DWORD)]
    for fn, types, result in signatures:
        fn.argtypes, fn.restype = types, result
    samples, modules = [], {}
    with (root / 'process.log').open('w') as log:
        process = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        snapshot = k.CreateToolhelp32Snapshot(4, 0)
        entry = ThreadEntry()
        entry.size = c.sizeof(entry)
        found = k.Thread32First(snapshot, c.byref(entry))
        tid = None
        while found:
            if entry.pid == process.pid:
                tid = entry.tid
                break
            found = k.Thread32Next(snapshot, c.byref(entry))
        k.CloseHandle(snapshot)
        thread = k.OpenThread(0xA, False, tid) if tid else None
        if not thread:
            process.terminate()
            raise c.WinError(c.get_last_error())
        raw = c.create_string_buffer(1232 + 16)
        address = (c.addressof(raw) + 15) & ~15
        start = time.perf_counter()
        try:
            while process.poll() is None:
                if k.SuspendThread(thread) == 0xFFFFFFFF:
                    break
                try:
                    c.c_uint32.from_address(address + 48).value = 0x100001
                    if k.GetThreadContext(thread, address):
                        samples.append([time.perf_counter() - start, c.c_uint64.from_address(address + 248).value])
                finally:
                    k.ResumeThread(thread)
                if len(samples) % 100 == 1:
                    handles = (w.HMODULE * 1024)()
                    needed = w.DWORD()
                    if ps.EnumProcessModules(int(process._handle), handles, c.sizeof(handles), c.byref(needed)):
                        for handle in handles[:needed.value // c.sizeof(w.HMODULE)]:
                            info, name = ModuleInfo(), c.create_unicode_buffer(4096)
                            if ps.GetModuleInformation(int(process._handle), handle, c.byref(info), c.sizeof(info)):
                                ps.GetModuleFileNameExW(int(process._handle), handle, name, len(name))
                                modules[info.base] = dict(path=name.value, base=info.base, size=info.size)
                time.sleep(0.001)
        finally:
            k.CloseHandle(thread)
        exit_code = process.wait()
    counts = collections.Counter()
    for _, rip in samples:
        for base, module in modules.items():
            if base <= rip < base + module['size']:
                counts[(module['path'], rip - base)] += 1
                break
    symbolizer = 'D:/Development/Mircosoft/VisualStudio/VC/Tools/Llvm/x64/bin/llvm-symbolizer.exe'
    resolved = []
    for (path, offset), count in counts.most_common():
        symbol = ''
        if 'CodeRepos' in path:
            result = subprocess.run([symbolizer, '--relative-address', '--obj=' + path, hex(offset)],
                                    capture_output=True, text=True)
            symbol = result.stdout.strip()
        resolved.append(dict(module=path, rva=hex(offset), count=count, symbol=symbol))
    for module in modules.values():
        path = Path(module['path'])
        module['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        if module['sha256'] is None:
            module['limitation'] = 'Temporary generated module removed by process shutdown'
    (root / 'samples.json').write_text(json.dumps(dict(command=cmd, exit_code=exit_code, tid=tid,
        caveat='Wall-clock main-thread RIP sampling, includes startup and sampling overhead; no cache counters.',
        samples=samples, modules=list(modules.values()), resolved=resolved), indent=2))
    if exit_code or not samples:
        raise SystemExit('Invalid sample run')


if __name__ == '__main__':
    main()
