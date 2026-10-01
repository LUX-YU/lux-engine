from pathlib import Path
import json,subprocess,hashlib,shutil,gzip,re
w=Path(__file__).resolve().parent;r=Path('E:/SyncForder/CodeRepos/lux-engine-p10q-structure');base='f7c27f9375cbf8dd8af37b30a6027a460de26213';sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip();out=w/'archive';out.mkdir(exist_ok=True);final=w/'final'/sha;build=Path('E:/SyncForder/CodeRepos/build/RelWithDebInfo')/('p10q-structure-'+sha[:12]);prefix=Path('E:/SyncForder/CodeRepos/install')/('P10Q-structure-'+sha[:12]);clean=Path('E:/SyncForder/CodeRepos/build')/('p10q-structure-clean-'+sha[:12]);digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def write(name,value):
 p=out/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
def copy(src,name):
 dst=out/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
def zipped(src,name):
 dst=out/name;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(gzip.compress(src.read_bytes(),mtime=0))
(out/'.gitattributes').write_text('* -text\n')
commands=json.loads((final/'commands.json').read_text());records=[]
for c in commands:
 copy(final/c['log'],'logs/'+c['log']);records.append(dict(c,log='logs/'+c['log']))
for c in json.loads((final/'sdk/commands.json').read_text()):
 copy(final/'sdk'/c['log'],'logs/sdk/'+c['log']);records.append(dict(c,log='logs/sdk/'+c['log'],implementation_sha=sha))
for p in final.glob('*-ctest-details.log'):copy(p,'logs/'+p.name)
copy(final/'ctest-details.log','logs/ctest-details.log')
for p in (final/'sdk').glob('*-ctest-details.log'):copy(p,'logs/sdk/'+p.name)
for p in (final/'sdk').glob('*-compile-commands.json'):zipped(p,'evidence/sdk/'+p.name+'.gz')
for p in (final/'sdk').glob('*reject_*.log'):copy(p,'evidence/sdk/'+p.name)
for p in (final/'public-headers').glob('*.log'):copy(p,'evidence/public-headers/'+p.name)
copy(final/'public-headers/commands.json','evidence/public-headers/commands.json')
for name in ['file-plan.json','retained-product.json','assertions-preserved.json']:copy(w/'audit'/name,name)
for name in ['baseline.json','tests-before.json','dependency-drift.json','dependency-drift-resolution.json','dependency-seed-source.json']:copy(w/name,name)
copy(final/'dependency-seed.json','evidence/dependency-seed.json')
for p in (w/'protected').iterdir():
 if p.is_file():copy(p,'protected/'+p.name)
# The mutable journal is frozen once. Earlier build errors/interruption remain evidence, never successful qualification.
for p in (w/'runs').glob('*.log'):copy(p,'development/'+p.name)
copy(w/'runs/commands.json','development/commands.json') if (w/'runs/commands.json').exists() else None
for old in (w/'final').iterdir():
 if old.name==sha:continue
 for p in old.glob('*'):
  if p.is_file():copy(p,'development/cold-attempts/'+old.name+'/'+p.name)
for p in w.glob('*.py'):copy(p,'construction/'+p.name)
for p in (w/'input').rglob('*'):
 if p.is_file():copy(p,'input/'+p.relative_to(w/'input').as_posix())
for p in (w/'preflight-sdk').rglob('*'):
 if p.is_file():copy(p,'development/preflight-sdk/'+p.relative_to(w/'preflight-sdk').as_posix())
for name in ['ledger.json','commands.json','dependency-drift-resolution.json']:
 if (w/name).exists():copy(w/name,'construction/'+name)
for p in (w/'audit').glob('*.json'):
 if p.name not in ['file-plan.json','retained-product.json','assertions-preserved.json']:copy(p,'evidence/audit/'+p.name)
for p in final.glob('regeneration-*.json'):copy(p,'evidence/'+p.name)
# Final actual rule fixtures, not the draft probes distributed with the attachment.
neg=sorted(build.glob('layering-boundaries*'),key=lambda p:p.stat().st_mtime)[-1]
for p in neg.rglob('*'):
 if p.is_file() and p.suffix in ['.log','.json'] and '/build/' not in p.relative_to(neg).as_posix():copy(p,'evidence/layering-negatives/'+p.relative_to(neg).as_posix())
compiler=sorted(build.glob('layering-compiler*'),key=lambda p:p.stat().st_mtime)[-1]
for p in compiler.glob('*'):
 if p.suffix in ['.log','.json','.cpp']:
  if p.stat().st_size>1_000_000:zipped(p,'evidence/compiler/'+p.name+'.gz')
  else:copy(p,'evidence/compiler/'+p.name)
for pattern in ['delivery-constraints*','operation-ownership*']:
 for folder in build.rglob(pattern):
  if folder.is_dir():
   for p in folder.glob('*.log'):copy(p,'evidence/constraints/'+folder.name+'/'+p.name)
copy(build/'editor-architecture/targets.json','evidence/targets.json')
zipped(build/'compile_commands.json','evidence/compile_commands.json.gz')
for p in (build/'.cmake/api/v1/reply').glob('*.json'):zipped(p,'evidence/file-api/'+p.name+'.gz')
copy(build/'install_manifest.txt','evidence/install_manifest.txt')
copy(build/'CMakeCache.txt','evidence/CMakeCache.txt')
# Exact files, including deletion, are verified against immutable blobs rather than producer paths.
paths=subprocess.check_output(['git','diff','--name-only',base,sha],cwd=r,text=True).splitlines();files=[]
for path in paths:
 result=subprocess.run(['git','show',sha+':'+path],cwd=r,capture_output=True)
 files.append({'path':path,'deleted':result.returncode!=0,'sha256':hashlib.sha256(result.stdout).hexdigest() if not result.returncode else None})
write('files.json',files)
rules=json.loads((r/'editor/tests/architecture/rules.json').read_text())['editor_layering'];graph=json.loads((build/'editor-architecture/targets.json').read_text());write('target-map.json',{'classifications':rules['targets'],'files':rules['files'],'shared_headers':rules['shared_headers'],'generated_roots':rules['generated_roots'],'actual':graph})
write('abstraction-map.json',{'static':[{'symbol':'deliverInput','choice':'One private synchronous constrained template, Material and Flow real callers','constraints':['lvalue callable','same expected<void,E>','no new owner/queue/payload']}], 'dynamic':[{'symbol':s,'choice':v} for s,v in {'IEditSession':'heterogeneous SessionStore; fixed code owner until destruction','ISaveSource':'open source role registration/revocation','IEncodeJob':'owned asynchronous domain encoding','IPreparedRebind':'prepared Save As domain adoption','DetachedView factory':'heterogeneous view and code owning unit','IArtifactStore':'real IO backend boundary'}.items()], 'rejected':['FrozenEncoder: only wrapper common; domain-specific snapshot/rebinding/encoding differ','Five aggregate libraries: directories express responsibility, real capabilities keep separate targets'], 'binary_changes':['editor_tasks new STATIC, TaskMonitor extracted from tasks_ui','editor_launch new STATIC, launchEditor extracted from editor_launcher','No global conversion; History/Session identity DLLs and metadata/plugin shared state remain']})
previous=json.loads((w/'tests-before.json').read_text());write('behavior-map.json',[{'old_test':x['name'],'new_test':x['name'],'assertions':'Original behavior preserved; 63 C++ test/support bodies token-identical after removing include directives and whitespace (assertions-preserved.json). Architecture fixture edges/provider paths updated without changing positive/reject/repair intent. Final detailed runtime output in logs/ctest-details.log.'} for x in previous['tests']])
write('scope-amendment.json',{'Linux':'NOT_RUN','system_IME':'NOT_RUN','ASan':'NOT_RUN','old_P10Q':'PARTIAL','old_performance':'PARTIAL','reason':'User explicitly waived Linux as current blocker and stopped old50k sample completion. Windows, UI/GPU, SDK and dependency negatives remain mandatory; no new fault deferred to old C IDs.'})
# Coverage observations are concrete responsibilities; log references point at this exact implementation run.
obs=[
'Original ProjectBuilder bytes/hash unchanged; tracked body identical at new path, user patch separately mapped; main and history snapshots unchanged.',
'Scene/Material/Flow share sole SessionStore/History; generation reuse/currentContent gates and code destruction preserved.',
'Three original standalone model SDK consumers edit/freeze/undo/redo using pure closure; installed link manifests recorded.',
'Original atomic mixed remove/recreate, field, node, signature and wiring tests unchanged.',
'Flow ID high water/sentinel; reload cleanup and READ gate callbacks retain original assertions.',
'Pure ProjectBuilder and Layout installed consumers exercise valid/invalid values with no Process; ProjectCatalog shared snapshots regressions retained.',
'Minimal SAVE_CORE consumer imports only persistence and pure contracts; typed model save roles remain separate activities.',
'Original concurrent save, SaveAs identity/history, ExportCopy and late stale adoption checks unchanged.',
'Original role self-revocation/recursive ack/accept-time completion collection tests run through real sources/TaskScope.',
'Write FIFO, Unknown, version succession and SharedBytes owner/limit regressions pass; no byte transport redesign.',
'Run tests drive actual SceneRuntime and inspect completed/cancelled/failed step results after heavy instance reclaim.',
'Material real compilation; Flow fixed product retry; operation eight negative compiles, pointer transfer and outcome ownership retained.',
'CPU TaskMonitor actual ExecutionRuntime installed consumer; multiple subscribers, FULL/CLOSED/resync and disconnected subscriber without cancellation. Existing TaskView multi-consumer test preserved.',
'Real file Workspace selected-only recovery/import and shared coordinator write/remove/marker tests preserved.',
'Viewport shared by Scene/Material; widget node IDs churn and renderer view-local highlight regressions pass.',
'Three CPU interaction tests and real Store cleanup BUSY/selection-only regressions preserved.',
'Ten original real Flow draft/Material+Flow queued-source scenarios use formal UI cache and signals, not hand-injected expected stamps.',
'Real detached Root/Pane mount/unmount, code lifetime, callback next batch and permanent close refusal tests unchanged.',
'Formal SceneView actual dual viewport GPU and Windows native input; old two explicit installed GPU modes separately exercised; system IME NOT_RUN.',
'Two actual tool instantiations and four constrained compilation negatives; no added public template API.',
'Existing dynamic roles/factories and shared identity DSOs retained; runtime ownership regressions unchanged.',
'Fresh dependency-only SDK; standalone changed public headers parsed under clang-cl C++20; designated generation outputs removed/rebuilt; second build no work.',
'Actual CMake/provider/compiler dependencies plus N01-N14 positive/reject/repair fixtures; unknown and transitive/LINK_ONLY/generated/template cases rejected.',
'518-file plan reconciled with Git blobs and current target/header providers; replaced physical paths absent, split legacy AssetSource/AssetSave remains registered until P12.'
]
coverage=[]
test_names=[x['name'] for x in json.loads((final/'test-names.log').read_text())['tests']]
patterns={
 1:r'^platform\.process_arguments$',
 2:r'^editor\.(sessions|three_actual_sessions)',
 3:r'^editor\.(scene_model\.content|material_model\.content|flowforge_model\.content)$',
 4:r'^editor\.(scene_model\.(atomic|mixed-)|material_model\.mixed-|flowforge_model\.mixed-)',
 5:r'^editor\.(scene_model\.read-|material_model\.(reload-|reading)|flowforge_model\.(ids-|reload-|input-))',
 6:r'^editor\.(project_creation|project_views|workspace\.(validation|effects|opaque))$',
 7:r'^editor\.persistence\.(models|decoded|execution)$',
 8:r'^editor\.persistence\.(save-as|close|close-key|identities|receipts)$',
 9:r'^editor\.persistence\.(r1-|r2-)',
 10:r'^editor\.persistence\.(write_coordinator|real_files|unknown|order|conflict|measure|capacity)$',
 11:r'^editor\.scene_execution\.',
 12:r'^editor\.compilation\.',
 13:r'^editor\.tasks\.monitor',
 14:r'^editor\.workspace\.',
 15:r'^(editor\.(canvas\.churn|projection\.highlight_backend)|render\.features\.view_binding)$',
 16:r'^editor\.(interaction_reclaim\.|scene_interaction|material_interaction|flowforge_interaction|interaction_run_identity)',
 17:r'^editor\.draft_source_',
 18:r'^(editor\.(detached_views|view_host)|ui\.root)$',
 19:r'^editor\.(scene_views_gpu|desktop_native_input)$',
 20:r'^editor\.layering\.delivery_constraints$',
 21:r'^editor\.(three_actual_sessions|view_host|compilation\.ownership_|persistence\.r1-)',
 22:r'^editor\.(inspector_codegen|component_elements|layering\.compiler_providers)$',
 23:r'^editor\.(layering\.(boundaries|compiler_providers)|architecture_current)$',
 24:r'^editor\.architecture_current$'
}
for i,text in enumerate(obs,1):
 evidence=['logs/ctest-details.log','behavior-map.json']
 if i==1:evidence=['baseline.json','protected/ProjectBuilder-relocated.patch','files.json']
 if i in [3,6,7,13,22]:evidence+=['evidence/install_manifest.txt','evidence/public-headers/commands.json']
 if i==19:evidence+=['logs/sdk/gpu-ui-ctest-details.log','logs/sdk/editor-scene-pane-ctest-details.log']
 if i in [20,23]:evidence+=['evidence/layering-negatives/results.json','evidence/compiler/results.json']
 if i==24:evidence+=['file-plan.json','retained-product.json']
 selected=[name for name in test_names if re.search(patterns[i],name)]
 assert selected,(i,patterns[i])
 coverage.append({'id':f'XL{i:02}','status':'PASS','observations':text,'tests':selected,'evidence':evidence})
groups=['sessions','editor-d2','external-feature','scene-ui','views-ui','scene-model','material-model','flowforge-model','persistence','scene-execution','projection-compilation','interaction-views','workspace','desktop-views','quality','layering-tasks','layering-save_core','layering-project','layering-layout','gpu-ui','editor-scene-pane']
receipt={'phase':'P10Q-structure','migration_stage':'P10Q','layering_mode':'STRICT','input_sha':base,'implementation_sha':sha,'status':'PASS','stop_after':'P10Q','continuation_authorized':False,'commands':records,'consumer_groups':groups,'coverage':coverage,'known_failures':{'C01':{'status':'FAIL','owner':'P12 full Workspace product application (P09 pure plans separate)'},'C03':{'status':'FAIL','owner':'P11 dynamic contributions/code lifetime'},'C04':{'status':'FAIL','owner':'P12 product teardown completion'}}}
write('receipt.json',receipt)
(out/'README.md').write_text(f'''# P10Q-structure 验收

状态：**PASS（本轮明确的 Windows 范围）**。停在 P10Q，等待复审；未进入 P11。

- 输入：`{base}`。
- 完整实现：`{sha}`；验收提交在其后单独形成。
- 门禁：`LUX_EDITOR_MIGRATION_STAGE=P10Q`、`LUX_EDITOR_LAYERING_MODE=STRICT`。
- 原工作区仍在输入提交，`ProjectBuilder.cpp` 用户字节未变且未纳入提交；main 未变。

## 实际交付

五层是职责目录，保留真实的细粒度构建边界，没有增加五个聚合库。

| 层 | 最终职责与唯一实现 |
| --- | --- |
| editing | 原 History、SessionStore、SessionState、身份与共同值；保留共享身份 DLL |
| authoring | Scene、Material、Flow 作者模型、纯 ProjectBuilder、Layout/Recovery 值与计划；不引入 Process |
| activities | 保存与文件发布、Run/Registry 编辑、投影、编译预览、项目/Workspace IO、TaskMonitor |
| workbench | Desktop/ViewHost/API、viewport、widgets、CPU interaction、三工具视图、项目和任务视图 |
| application | `launchEditor()` 的唯一进程启动实现；现有 Launcher/App 直接消费 |

纯分区解码收回 World storage：ScenePackage 直接解码已拥有的内存卷，Process 异步范围读取复用同一个解码核。删除 ScenePackage 的 Process 运输接线及传递依赖。多卷、多 extent、摘要、限制、取消、未知载荷与异步结果对照已运行。

运行实例的 SceneEditing 仍有真实 RunStore 消费者，迁到 activities，未复制历史或另造桥。TaskMonitor 拆为不含 UI 的 `editor_tasks`。`ViewInfo` 留在纯 contracts，关闭错误归 view_api。三个模型的 PersistenceAccess 留在领域内部。生成器、support、安装 provider 和调用方一起迁移，逻辑 include、namespace、原包名与库名保持。

共享 `deliverInput()` 使用左值可调用、同一 `expected<void,E>` 的 concept；Material/Flow 生产调用与四类编译负例均运行。异构会话、保存源、encode job、重绑定与工厂仍保留必要动态边界。没有新 Manager、Runtime、事件总线或服务定位器。

新增的两个实际 STATIC provider 是 `editor_tasks` 和 `editor_launch`。History/Session/Metadata 的共享身份及状态边界未改为重复静态副本；原 tasks_ui、editor_launcher 仍有实际实现，未留下空转发 target。

## 最终验证

所有最终命令绑定上述完整实现 SHA，使用独立 clean tracked clone、新依赖种子和新 SDK 前缀。

| 验证 | 结果 |
| --- | --- |
| Editor 全量 all -j4 -- -k0、二次无工作、完整 CTest | PASS，209/209 |
| CPU native 独立配置、全量构建及 CTest | PASS，183/183 |
| PLAYER 无 Editor 编译单元、全量构建及 CTest | PASS，12/12 |
| 原 14 组 SDK + quality + 4 个最小层消费者 + 2 个显式 GPU 消费者 | PASS，21 组，全部使用新安装 SDK |
| 新 SceneView 双视口 GPU/验证层、Windows 原生输入 | PASS；两个旧 GPU 模式另行执行 |
| N01–N14 实际依赖正例、指定规则拒绝、去边恢复 | PASS，含传递、LINK_ONLY、生成头、模板和未知 imported target |
| 4 项交付 concept 负例、8 项 operation 特殊成员负例 | PASS，生产公共头与实际实例化 |
| clang-cl C++20 消费者、逐个改动公共头独立解析 | PASS |
| 删除指定生成输出后重建及第二轮无工作 | PASS；重建字节 hash 一致 |

原 204 个行为名称全部保留；63 份原 Editor C++ 测试/支持文件去除 include 与空白后的 token 一致。其余路径与规则夹具的等价调整见行为映射和实际日志。测试总数仅作索引，XL01–XL24 的具体行为、测试名及证据见 [receipt.json](receipt.json)。

首次失败没有隐藏：外部依赖 seed 的文本路径差异在核对全部外部文件后定界；早期冷构建为补齐丢失的 README 契约主动中止；下一次完整运行暴露一处旧生成器测试路径，保留原四个断言、修正路径后在最终 SHA 全量重跑。开发期失败/中止日志在 `development/`，不计最终通过。

## 文件、依赖与暂留项

- [file-plan.json](file-plan.json)：518 个原 Editor 文件的原 blob、落点、实际 provider、消费者和最终动作。
- [files.json](files.json)：全部实际增删改文件及固定 Git 对象 hash，含引擎/CMake/消费者。
- [target-map.json](target-map.json)：实际 target 图、文件/生成头 provider 与层分类。
- [abstraction-map.json](abstraction-map.json)：静态/动态边界与未采用抽象的理由。
- [retained-product.json](retained-product.json)：旧产品每个暂留 target 的消费者和 P11/P12 期限。新正式路径不依赖这些旧岛。
- [behavior-map.json](behavior-map.json)、[assertions-preserved.json](assertions-preserved.json)：原行为与断言保留证据。
- [evidence/audit/protected-final.json](evidence/audit/protected-final.json)：原工作区、main、用户字节和历史快照核对。

`ProjectBuilder.cpp` 只迁移 tracked 原体；用户补丁的新位置是 `editor/authoring/project/src/ProjectBuilder.cpp`。已保存原字节和 [重定位补丁](protected/ProjectBuilder-relocated.patch)，对新实现执行 `git apply --check` 通过，未替用户应用。

唯一可变施工记录仍在原工作区 `.internal/editor-redesign/layering/`。本目录是冻结快照；`construction/` 中的账本和脚本不作为第二份可写账本。

## 明确未通过或未执行的既有范围

Linux、系统 IME、ASan：NOT_RUN；本轮未执行 Android 构建。未改 modules 公共头，无三前缀同步事项。

原 P10Q 与旧 50k 深链性能项仍为 PARTIAL；按用户要求未补慢样本。本轮没有改写旧结论。

C01/C03/C04 以原断言再次得到 FAIL：C01 完整 Workspace 产品应用归 P12；C03 动态贡献/代码寿命归 P11；C04 产品关闭结清归 P12。未将本轮新问题挂到这些旧编号。

## 归档验证

使用 `editor/tests/architecture/validate_layering_evidence.py --source <含实现 Git 对象的仓库> --archive <本目录>`。取证使用归档相对路径和固定实现 Git 对象，不需要日志中的生产机器绝对路径。中文/空格路径迁移、实际日志缺失/篡改拒绝及恢复的执行记录见 `archive-probes/`。
''',encoding='utf-8')
write('artifacts.json',[{'path':p.relative_to(out).as_posix(),'sha256':digest(p)} for p in sorted(out.rglob('*')) if p.is_file() and p.name not in ['artifacts.json','receipt.json']])
print('Archive prepared for',sha,'commands',len(records),'files',len(files),'artifacts',len(json.loads((out/'artifacts.json').read_text())))
