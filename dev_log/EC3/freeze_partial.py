"""Freeze the user-requested push checkpoint; do not turn blocked validation into PASS."""
from pathlib import Path
import hashlib, json, shutil, subprocess

w = Path(__file__).resolve().parent
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
a = s / 'dev_log/EC3'
assert not a.exists(), 'Never replace a frozen receipt'
cfg = json.loads((w / 'final-config.json').read_text())
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
assert sha == cfg['implementation_sha']
assert not subprocess.check_output(['git','status','--porcelain'], cwd=s).strip()
ledger_path = w.parent / 'migration-ledger.json'
ledger = json.loads(ledger_path.read_text())
ec3 = ledger['ec3']
ec3['push_checkpoint'] = {'requested_by_user': True, 'acceptance': 'PARTIAL_VALIDATION_BLOCKED',
    'not_a_test_waiver': True, 'implementation_sha': sha, 'archive': 'dev_log/EC3',
    'dependency_branch': 'codex/ec3-inspector-ir', 'dependency_sha': cfg['dependency_sha']}
ec3['next_entry'] = 'EC3 review / resolve recorded Defender detection, then remaining C9 qualification; no next stage.'
ledger_path.write_text(json.dumps(ledger, ensure_ascii=False, indent=2)+'\n')
a.mkdir()
for f in w.iterdir():
    if f.is_file(): shutil.copy2(f, a/f.name)
for folder in ['logs', 'before', 'input']:
    shutil.copytree(w/folder, a/folder)
for folder in w.glob('SDK 中文 *'):
    dest = a/folder.name
    dest.mkdir()
    for f in folder.iterdir():
        if f.is_file() and f.suffix in {'.json','.log'}: shutil.copy2(f,dest/f.name)
# This is the frozen EC3 node of the sole mutable ledger, not another editing surface.
(a/'ledger-snapshot.json').write_text(json.dumps(ec3,ensure_ascii=False,indent=2)+'\n')
(a/'.gitattributes').write_text('* -text\n')
baseline = json.loads((w/'baseline.json').read_text())
old = subprocess.check_output(['git','rev-parse',baseline['head']+':dev_log'],cwd=s,text=True).strip()
assert old == subprocess.check_output(['git','rev-parse',sha+':dev_log'],cwd=s,text=True).strip()
receipt = {
    'stage':'EC3', 'status':'PARTIAL_VALIDATION_BLOCKED', 'architecture_mode':'STRICT',
    'baseline_sha':baseline['head'], 'implementation_sha':sha, 'original_dev_log_tree':old,
    'dependency':{'repository':'https://github.com/LUX-YU/lux-cxx','branch':'codex/ec3-inspector-ir',
                  'sha':cfg['dependency_sha'],'status':'54 tests/all/no-work/install passed; engine integration matrix incomplete'},
    'push_requested_not_test_waiver':True, 'source_tests':'final-partial-results.json',
    'completed_final_checks':['clean tracked snapshot','EC3 STRICT configure','all -j 4 -- -k 0',
        'second build no work','231 distinct tests across recorded partial runs; includes 20 dependency tests'],
    'not_complete':['complete CTest (4 loader timeouts, 1 plugin-load failure, 5 blocked tests)',
        'final PLAYER','full fresh SDK installation and consumers','final installed standalone clang-cl headers',
        'generated Run control gesture extension (prepared only)','full final explicit presentation GPU modes'],
    'quarantine':'Windows Defender isolated lux_engine_scene_composition.dll; false positive not confirmed',
    'original_workspace':str(w.parents[2]), 'review_workspace':str(s),
    'qualification_workspace':cfg['source'], 'sdk_prefix':cfg['prefix'],
    'sdk_prefix_state':'DEPENDENCIES_ONLY_ENGINE_INSTALL_NOT_COMPLETED',
    'user_patch_applied':False, 'user_patch_sha256':baseline['patch_sha256'],
    'user_patch_original_path':'editor/project/src/ProjectBuilder.cpp',
    'user_patch_current_path':'editor/authoring/project/src/ProjectBuilder.cpp',
    'main_modified':False,
    'inherited':{'EC2':'PARTIAL; EC2 R1 scoped review accepted; F-EC2-01 closed',
        'EC2_native_input':'NOT_RUN_USER_DEFERRED','P12':'PARTIAL_USER_WAIVER',
        'Linux':'NOT_RUN','system_IME':'NOT_RUN','sanitizer':'NOT_RUN','old_performance':'PARTIAL_NO_MORE_SAMPLES'},
    'stop_after':'EC3 review; no main merge, branch deletion or release',
}
(a/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n')
(a/'README.md').write_text('''# EC3 推送检查点：PARTIAL / 验收受阻

本记录按用户“推送吧”的要求冻结。它不表示 EC3 已通过，不把该要求解释为免除剩余验收。
实现与此记录分别提交；唯一可变施工账本仍在原工作区 `.internal/editor-redesign/`。

- 实现 SHA：`'''+sha+'''`。
- 用户应查看：`E:/SyncForder/CodeRepos/lux-engine-ec2`。
- 原工作区：`E:/SyncForder/CodeRepos/lux-engine`，用户 ProjectBuilder 修改保持原字节，未应用、未提交。
- 对应新路径：`editor/authoring/project/src/ProjectBuilder.cpp`。
- lux-cxx 依赖：`'''+cfg['dependency_sha']+'''`，分支 `codex/ec3-inspector-ir`。

## 已落地的实现

固定声明归实际模块，动态文字有冻结 backing 和代码 pin；命令派发复用数值索引和原句柄。
项目保存、插件选择、最近项目、工作区和恢复政策由实际活动或窄工作台用例承担。
原贡献体系扩充设置和 V9 SDK，设置草稿区分来源、生效及落盘；实际窗口使用环境解析与普通矩形保留。
相机沿原 ECS/RenderSystem 链，仅补显示输出与输入一致性，避免未变化值的重复 patch。
Inspector 删除生产 Python emitter，使用原 lux-cxx MetaUnit、数组 IR 和 inja 作者/Run 模板。
详见 `ledger-snapshot.json` 的 149 项输入、60 个验证主题、实际文件/方法/消费者及 owner 映射。
这些映射保留未完成资格，不依靠数量推定 PASS。

## 实际结果及限制

最终独立干净检出完成 EC3 + STRICT 全量构建及二次无工作。
231 个不同测试通过，分布在原完整运行的已完成部分、20 项依赖检查、补充运行的 82 个通过项中。
这不是一次完整 CTest 通过；逐项结果见 `final-partial-results.json`。
原完整运行在测试期间失去 DLL：4 项进入 main 前超时，之后停止；5 项没有继续运行。
补充运行的场景配置测试还发生 1 项插件加载失败，实际 LoadLibraryEx 返回 126，
插件传递依赖同一个缺失 DLL。原失败输出与加载器诊断均保留，没有改判成通过。

Windows Defender 的隔离记录位于 `final-defender-events.txt`；是否误报尚未确认。
没有修改安全设置、恢复隔离文件、设置排除项或重构该 DLL 来避开检测。
最终 PLAYER、完整新 SDK 安装/消费者、安装头第二编译器和生成 Run 控件手势资格仍未完成。
`run-controls.fragment.cpp` 和相关脚本是尚未执行的准备材料，不是运行证据。
当前新 SDK 前缀只有已准备的第三方依赖，不能当作完整可交付 Engine SDK。

较早的 SDK、插件、生成增量和两个呈现模式成绩按 `commands.json` 的原 SHA/工作区指纹记录，
不提升为最终实现重跑。源构建中通过的新双视口 GPU 与实际设置窗口是本轮结果；
原生输入、Linux、系统 IME 和旧慢测延期保持原范围。

## 核验与继续入口

`python verify.py --repo <含实现 Git 对象的仓库>` 只读验证归档哈希、真实命令、源码与历史树。
它验证 PARTIAL 收据的真实性，不宣布行为验收通过；不读取生产机器绝对路径。
恢复验证时使用新日志标签，不覆盖本次隔离、失败、勘误或历史记录。
停在 EC3 等待复审，不进入下一阶段。
''',encoding='utf-8')
print('Frozen files before verifier/manifest:',len([f for f in a.rglob('*') if f.is_file()]))
