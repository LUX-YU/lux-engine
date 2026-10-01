from pathlib import Path
import json, subprocess

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-p11')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
f = w / 'final' / sha
records = json.loads((f / 'commands.json').read_text())
by_name = {r['name']: r for r in records}
for name in ['build', 'no-work', 'ctest', 'cpu-ctest', 'player-ctest', 'sdk-all',
             'clang-public-headers', 'regenerate-no-work']:
    assert by_name[name]['exit_code'] == 0, name
abi = json.loads((f / 'abi-independence/result.json').read_text())
assert abi['runtime_identity_unchanged'] and abi['editor_identity_changed']
counts = {}
for key, file in [('windows', 'test-names.log'), ('cpu', 'cpu-test-names.log'), ('player', 'player-test-names.log')]:
    counts[key] = len(json.loads((f / file).read_text())['tests'])
assert counts == {'windows': 222, 'cpu': 196, 'player': 12}, counts
sdk = json.loads((f / 'sdk/commands.json').read_text())
groups = sorted(r['name'].removesuffix('-ctest') for r in sdk if r['name'].endswith('-ctest'))
assert len(groups) == 24 and all(r['exit_code'] == 0 for r in sdk)
ledger_file = w.parent / 'migration-ledger.json'
ledger = json.loads(ledger_file.read_text(encoding='utf-8'))
r1 = ledger['closeout']['revision_r1']
r1.update(status='WINDOWS_VERIFIED_PENDING_ARCHIVE', qualification=counts,
          sdk_groups=groups, implementation_sha=sha, archive='dev_log/P11-R1/receipt.json',
          scope={'Linux': 'NOT_RUN', 'system_ime': 'NOT_RUN', 'sanitizer': 'NOT_RUN',
                 'old_50k': 'Original PARTIAL unchanged'},
          original_worktree_unchanged=True, user_patch_applied=False)
ledger_file.write_text(json.dumps(ledger, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
report = w / 'report.md'
text = report.read_text(encoding='utf-8')
assert '## 最终执行结果' not in text
text += '''
## 最终执行结果

- 固定实现 SHA 的独立 clean clone：全量构建通过，第二轮无新增工作；P11 + STRICT。
- Windows 全量 CTest 222/222，CPU native 196/196，PLAYER 12/12；保留原 213 个命名行为及断言。
- 全新 SDK 前缀 24 组消费者全部通过；真实 V7 DLL 两种最后 owner 模式、R11 的真实保存/贡献消费者及两个旧显式 GPU 模式均通过。
- 新双视口 GPU 隔离/退休和原生鼠标/键盘/焦点/Inspector 输入通过，validation_errors=0。系统 IME 未测。
- clang-cl 公共头与质量消费者通过；修改的两个安装公共头独立按 C++20 解析。
- 指定 Inspector 生成输出删除后重新生成字节一致，第二轮无新增工作。
- 实际安装 Editor ABI 指纹已改变，运行 SDK 身份不变；隔离 Editor 版本变更也证明运行 ABI 不受影响。

缺失、篡改、错误源码 SHA 和中文/空格路径迁移资格见 `archive-probes/results.json`。首次失败、测试夹具修正和修复前七个真实 SDK 失败保持在归档中。
'''
report.write_text(text, encoding='utf-8')
print('Recorded final qualification', sha, counts, len(groups))
