"""Update the one construction ledger only after actual final qualification."""
from pathlib import Path
import json, subprocess, sys

w = Path(__file__).resolve().parent
s = Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=s, text=True).strip()
f = w / 'final' / sha
records = json.loads((f / 'commands.json').read_text())
by_name = {x['name']: x for x in records}
for name in ['build', 'no-work', 'ctest', 'cpu-ctest', 'player-ctest', 'sdk-all', 'clang-public-headers',
             'regenerate', 'regenerate-no-work']:
    assert by_name[name]['exit_code'] == 0
abi = json.loads((f / 'abi-independence/result.json').read_text())
assert abi['runtime_identity_unchanged'] and abi['editor_identity_changed']
subprocess.run([sys.executable, w / 'audit.py'], check=True)
behavior = json.loads((w / 'behavior-map.json').read_text())
for row in behavior:
    row['status'] = 'PASS'
    row['implementation_sha'] = sha
(w / 'behavior-map.json').write_text(json.dumps(behavior, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
ledger = w.parent / 'migration-ledger.json'
d = json.loads(ledger.read_text())
d['closeout'].update(status='PASS_AWAITING_REVIEW', batch='G', implementation_sha=sha,
                    final_evidence='dev_log/P11/receipt.json', continuation_authorized=False,
                    sdk_lifetime_regression='Actual K DLL crash retained; final fresh SDK both last-owner modes pass',
                    cold_attempt='9b8627fa missing exact ObjectEvent header edge retained; final tracked cold build/no-work pass',
                    Windows='PASS', Linux='NOT_RUN', system_ime='NOT_RUN',
                    original_product='lux_editor still uses editor/app until independently authorized P12')
ledger.write_text(json.dumps(d, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
tests = len(json.loads((f / 'test-names.log').read_text())['tests'])
files = json.loads((w / 'files.json').read_text())
header_records = json.loads((f / 'public-headers/commands.json').read_text())
sdk = json.loads((f / 'sdk/commands.json').read_text())
groups = sorted(x['name'].removesuffix('-configure') for x in sdk if x['name'].endswith('-configure'))
report = f'''# P11 正式命令、不可变贡献与内容工厂验收

P11 Windows 范围 **PASS，等待独立复审**。未进入 P12，安装的 `lux_editor` 仍使用旧产品装配。

- 实现提交：`{sha}`；验收材料在其后的独立提交中。
- 用户应查看：`E:/SyncForder/CodeRepos/lux-engine-p11`，分支 `codex/p11-closeout`。
- 推送目标：`codex/editor-redesign-v4`，正常快进推送，不修改 main。
- 原工作区仍为 `E:/SyncForder/CodeRepos/lux-engine`，保留 `f7c27f9375cbf8dd8af37b30a6027a460de26213` 和用户的 ProjectBuilder 修改。
- 用户补丁 **未应用、未提交**。原字节、diff 及新路径补丁见 `protected/`；新路径是 `editor/authoring/project/src/ProjectBuilder.cpp`。

## 已交付的责任闭包

| 层/提供者 | 本阶段的正式能力及唯一 owner |
|---|---|
| editing/contracts、SessionStore | 拥有型代码租约及现有身份。Store 仍独占作者会话；无第二 History 或保存基线。 |
| authoring/configuration | 从旧 metadata 迁出唯一 ConfigurationValue、EditorReflection 原体。纯配置不依赖 UI。 |
| activities/commands | 固定目标/参数/来源策略、不可变注册、调用期 pin 和有界派发。保持 query/execute 分离。 |
| activities/sessions + 三领域工厂 | worker 读取/解码拥有数据；owner 预留和构造；隐藏会话与保存角色在无回调发布点共同可用。 |
| SaveService | 唯一 prepare/commit/abandon 注册算法，立即 registerSource 复用它；完成交付及 P05 R1/R2 纪律保留。 |
| workbench/desktop | CommandMenu/快捷键产生固定 invocation；工厂只返回完整 DetachedView，ViewHost 独占挂载窗口。 |
| application/extensions | 不可变贡献批次、原反射 draft、V7 Editor 出口、真实内置装配，沿用 engine PluginLibrary。 |

三模型工厂都经过真实文件读取、codec、owner 构造、编辑、Undo/Redo、Save/SaveAs/Export 与关闭。
新的 Runtime 独立消费者没有导入 Editor target；隔离改动 Editor 契约版本只改变 Editor ABI 身份。
新增命令、工厂、配置与扩展生产模块的静态检查未发现独立平台装载调用或 C++23 API；DLL 装载仍走原 PluginLibrary，平台运行测试仍有明确条件。这不代表 Linux 构建或运行已验证。

## 删除与 P12 交接

本阶段涉及 {len(files)} 个 tracked 文件；逐文件 hash/增改删见 `files.json`。
已删除旧 metadata 中 ConfigurationValue/EditorReflection 两组定义及旧插件 ConfigurationForm 原体；全部消费者改用唯一正式实现。
没有增加 Old/New 回退，没有重做 Process、SceneRuntime、History 或 Renderer。

旧 metadata、plugins、context、app 尚有真实旧产品消费者，精确文件、符号、provider、include/link 消费者和替代批次见 `source-map.json`、`removal-plan.json`。
包括旧 Pane/AssetEditor/Command 登记、V6 装载与三组 sidecar、旧菜单和产品启动装配；它们最迟在 P12 产品切换时删除。
特定 Physics2D 配置控件仍由旧 sidecar 消费，未把它宣称为已迁移。所有旧主菜单功能的正式/延期映射见 `command-map.json`。
导入活动与运行实例 SceneEditing 仍是正式活动，不属于待删桥。

## 实际验证

- 同一实现 SHA 的独立 clean clone：P11 + STRICT，全量 all / -j 4 / -k 0；二次 no work。
- 完整 {tests} 项 CTest，保留前置 209 项名称及语义映射；CPU 独立配置和 PLAYER 全量通过。
- 全新 SDK 前缀，{len(groups)} 组消费者配置、构建、二次 no work 与运行通过；真实三模型、外部 V7 DLL、独立 Runtime DLL 均使用安装公共接口。
- 五个不兼容 DLL 在贡献回调前拒绝；六类新实际依赖负例及原禁边/八项 operation 特殊成员负例保留。
- {len(header_records)} 个修改公共头通过安装前缀下 clang-cl C++20 独立解析；现有第二编译器质量消费者通过。
- 正式 SceneView 双视口 GPU、验证层和新 DesktopShell 原生输入通过；Undo 实际经正式命令调用。
- 原两个显式 GPU 模式分别通过，生成输出删除后正确再生成，第二轮无新增工作。
- 原始断言/源码 blob 对照见 `regression-source-map.json`；不是以测试数量代替行为证明。

## 实际发现与保留证据

开发期真实安装 SDK 曾在最后 DLL 卸载后销毁弱引用控制块时崩溃，cdb 栈和首次失败输出保存在 `failures/`。
修复将保存注册控制块分配绑定到 SaveService 构造所在模块，并由接收模块持有开放条目的外部 code owner；未通过无限保活 DLL 绕过问题。
最终外部扩展同时验证有配置控件保活和无额外配置保活两种情形，结清结果后真实卸载，再销毁过期弱引用。

最终复查还发现共享未变条目构造新快照时，接收端包装可能形成历史链。真实 CommandRegistrySnapshot 回归先证实旧包装不能释放；修复后连续一万次更新保持同一个条目、不保留旧包装，并再次经过实际 DLL 卸载验证。中止的中间提交冷构建记录保留，不计入最终资格。

独立持久化 SDK 消费者曾发现三领域包在生成安装配置后才登记工厂依赖，导致单独 find_package 失败。已将依赖登记移至安装生成之前；最终保存消费者及三领域分别独立配置全部通过，修复前失败保留。

首次冷配置缺少精确 ObjectEvent 公共头规则，失败日志保留；补齐真实合法依赖后通过。没有放宽整层或旧 Context 白名单。

| 历史项/平台 | 当前状态 |
|---|---|
| C03 | 正式新路径与旧产品活动 probe 均 PASS；原历史 FAIL 快照保持原字节。 |
| C01 | 保持 FAIL；完整布局应用责任仍在 P12。 |
| C04 | 保持 FAIL；仍是原启动/菜单连接失败契约，未换成退出测试。 |
| Linux、系统 IME | NOT_RUN；原生键鼠测试不代表 IME 候选/组合实测。 |
| sanitizer | NOT_RUN；未用消费者部分插桩宣称完整资格。 |
| 旧 50k 深链 | 原 PARTIAL 保持，本阶段不补样本。 |

## 证据可迁移性

`receipt.json` 绑定实现 SHA，命令含 argv、目录、起止时间、退出码与相对日志哈希；路径只是运行事实，验证不依赖生产盘符。
`verify.py --source <含实现 Git 对象的仓库> --archive <本目录>` 检查实际归档和固定 Git 对象。
`archive-probes/` 保存中文/空格路径迁移正例、缺失及篡改真实日志拒绝、错误实现 SHA 拒绝和恢复后正例。
唯一可变施工账本仍在原工作区 `.internal/editor-redesign/`；这里是本阶段冻结快照。

复审通过并另行授权后才执行 P12。
'''
(w / 'report.md').write_text(report, encoding='utf-8')
print('Final qualification mapped; immutable freeze is next', sha)
