# EC2 收尾记录

状态：**PARTIAL：实施完成，原生输入验收由用户要求延后**。EC2+STRICT 的其余本轮 Windows 检查通过；不是全部必测已通过。保留这次未测，后续仍须运行源码与安装 SDK 两项原生输入检查。

- 前置验收：`248adc4576943cab83976afd8d1d5f31b63b70a9`。
- 实现：`dd54847ee281f5e96adf4ebeba2f6c06bc858515`，分支 `codex/editor-redesign-v4`。
- 查看工作区：`E:/SyncForder/CodeRepos/lux-engine-ec2`。
- 干净资格检出：`E:/SyncForder/CodeRepos/lux-engine-ec2-qualified`。
- 原工作区 `lux-engine`、main 和 ProjectBuilder 用户修改保持原样。补丁**未应用**；新路径为 `editor/authoring/project/src/ProjectBuilder.cpp`。

## A. EC1 继承和补充审计

EC1 的唯一 EditExecutor/History、开放内容路由、V8、SessionPreparation 和外部 Skeleton 保留。没有重建万能 Context、另一套历史、执行器、Renderer 或发布器。补充责任审计 RA01–RA09 分别落入本轮 R1–R4；RA10 继承原实现。逐项前后责任、调用链、检查有效范围见 `architecture-notes.md`。

基线 223 个登记项在最终配置中均保留，新增 13 项，详见 `behavior-map.json`。测试名称仅作索引；实际断言位于固定 Git 提交，运行明细另存，未以测试数量替代语义核验。

## B. 数据、权限、执行与呈现

| 数据或责任 | 本轮唯一 owner／算法 | 删除或收敛 |
|---|---|---|
| 固定项目发布计划 | ProjectPublicationPlan 的不可变共享数据 | 去掉 worker 修改计划、释放 Storage 占用的入口 |
| 发布占用权 | move-only PreparedProjectPublication 与原 Storage reservation | 仍由原 ProjectPublicationOperation/WriteCoordinator 发布；没有第二发布器 |
| 打开准备 | PreparedProjectOpen | 删除 ProjectOpenData 名称、旧公开头和旧调用 |
| Scene 配置 | authoring 的 draft；activities 的准备算法 | 删除 SystemElement/Impl 中重复描述构造算法 |
| cooked 产物发布 | ArtifactPublicationOperation | Application 不再执行打包、文件票据和 manifest 采用算法 |
| 状态与操作界面 | ResultsView、WorkspaceView | 删除绑定整个 ApplicationImpl 的 ResultsPane/WorkspacePane |
| 编译与预览 | 固定 CompiledMaterial/CompiledFlow；独立 PreviewAdoptionKey、MaterialPreview | 删除目标混入编译输入及 MaterialPreviewStore 旧接口 |
| Workspace | 固定旧输入的纯转换；WorkspaceMigration 的读取/复查；原 Store/Coordinator 发布 | 删除 layoutResult 的查询附带目录 IO |
| 模型导入 | ModelImporter、纯 ModelImportRecipe codec、原 ModelCooker | 删除名不副实的 AssetImporter 入口和重复 recipe 编解码 |

源、头、生成输入、CMake、安装与全部 include 消费者见 `file-actions.json`；Git 字节校验见 `files.json`。ProjectCreation 的多文件 journal 与运行期不可变包发布语义不同，按 R0 决定保留前者，不机械删除崩溃恢复。旧格式只读迁移仍保留。

## C. 游戏脚本实际支持范围

新增 engine 原生资产能力，借用原 ExecutionRuntime 和固定 AssetReadPort；脚本只接触有界结果值和全域/代次句柄。资产仍由原 VFS/Process/codec 读取解码。

实际提供：raw read、describe、最多 256 字节的范围查询、显式 release；独立 Skeleton typed read、骨骼数量和父索引查询；原 Delay；明确开放组件的读/延迟 patch。完整 ID 通过有界 opaque Lua 值传输。非平凡资产、code pin、取消和结果保管留在 native scope。

原 ScriptSystem 在铸造实例身份后准备 binding，退休时先 revoke，再销毁 backend/binding/code。已接受完成在原有界记录内等待 ingress 可接收，暂停只停止规则演化。worker 不持 Registry、VM 或 live Session。

不声称已绑定全部物理、动画、音频、输入或渲染能力。ScriptRuntimeHost 的 backend/span/resolver 仍是明确借用，组合方负责其寿命；Run 的 shared host 不暗中延长借用。没有自动从 Editor 全局服务查找脚本。

## D. 真实链路与范围

同一 packaged Lua 脚本经过 ScriptAbility 生成、原 LuaBoundary、ScriptSystem、VFS/pak、CPU 解码、Delay、原 ECS command barrier，分别在 SceneRuntime、PLAYER 和 Editor Run 中运行。Run 检查完整作者编码、current、observed、dirty 和 binding 未被修改。

XEC2-32 另有真实安装消费者的联合测试：同一 packaged Lua、同一个 Run 实例连接两个 GPU SceneView；两者具有不同视口资源和图像。暂停时关闭左窗不停止 Run，右窗继续呈现；恢复后脚本把 Counter 从 7 改为 9，全部 continuation/awaitable 结束。停止 Run 后 prepared slots 为 0，右窗关闭、GPU 退休，验证层错误为 0；作者完整编码、current、observed、dirty、binding 均不变。这是实际 GPU 输出与生命周期测试，不声称人工画面或 IME 验收。

联合夹具两次首次失败均保留：最初只读计数误用可写借用遇到正常 BUSY；随后在组件命令已采用、协程末尾尚未完成时过早检查终态。夹具改为正式 const 借用，并等待原 continuation/awaitable 归零，未删除断言、未修改生产 Runtime。

原生插件测试使用实际 DLL、非平凡结果及可观察 deleter。开发期曾重现最后 code pin 卸载仍在返回的插件控制块；修复为 native DLL 创建该控制块。修复前日志 `r7-real-owner-results` 保留。拒绝、取消、晚到结果和最后 owner 的清理继续通过原机制完成。

## E. 工程与验证

最终配置为 **EC2 + STRICT**。首次资格来自独立 clean tracked clone。第三方版本沿用 EC1；新 SDK 前缀预先仅放第三方产物，复制的第三方 CMake 包绝对前缀重定位有原始/新哈希，未复制旧 engine 头和库补齐构建。

### 最终运行汇总

| 项目 | 实际结果与证据 |
|---|---|
| clean tracked clone，EC2+STRICT 首次全量构建 | PASS，1409 步；第二轮 `ninja: no work to do`，`final-build`／`final-no-work` |
| 最终主 CTest | 235/235 执行项 PASS；236 个登记项中仅原生输入未运行。原 223 个名字全部保留，新增 13 项 |
| PLAYER | 首次全量构建、二次无工作、19/19 PASS；504 个编译单元无 Editor 源/include；实际 DLL 闭包无 Editor |
| 原 SDK 消费者 | 24 组 configure/build/no-work/test PASS，含原八项 operation 特殊成员编译负例 |
| EC2 SDK | NATIVE、LUA、SCENE、ACTIVITIES、PANELS 五组 PASS；原生 import/DLL 闭包无 Lua/Editor/GPU/LLVM |
| EC1 骨骼扩展 | HEADLESS/WINDOW/APP 三组实际 DLL 消费者 PASS |
| 外部产物 | 实际 DLL 的 IArtifactSource、自定义 cooked 类型经过原发布链；Source S1、Unknown、manifest 失败与 retry PASS |
| 脚本双视口 | 同一 Run／同一 packaged Lua／两 GPU SceneView 的关闭、恢复、停止联合测试 PASS，验证层错误 0 |
| 原 GPU 路径 | 新双 SceneView readback、高亮隔离、两种旧显式模式 PASS，详见主 CTest 明细 |
| 编译与边界 | 36 个修改公共头通过 clang-cl C++20；12 个实际 EC2 非法边正例/拒绝/恢复；三项计划不可变负例及已移除安装头负例 PASS |
| 源／安装 | 156 个变动路径逐字节登记；无旧名称、旧头及旧前缀引用；5 个 modules 头同步 3 个前缀，共 15 个文件核验 |
| 归档门禁 | 搬迁到中文／空格路径通过；真实构建日志缺失、篡改均拒绝。门禁确认记录有效，阶段结果仍是 PARTIAL |
| 原生输入（源码／SDK） | **NOT_RUN_USER_DEFERRED**。用户回答“稍后再做”；不是失败改判，也不是永久免验 |

原有 SDK 身份审计使用了不存在的文件名 glob，首次失败原记录、输出和脚本已留在 `before/` 与日志中；修正为核对正式 `share/lux-engine/plugins/LuxPluginSdk.cmake`，与本次构建字节一致，再运行最终审计。没有为通过删除身份检查。

独立头检查是 clang-cl C++20 解析，SDK 是真实安装编译/链接/运行，依赖负例是实际 configure/reject/repair，GPU 是真实设备测试；四者分开记录。原有编译警告保留，没有把通过描述为“零警告”。

## F. 有界成本和寿命

最终原生测试已观察：20,000 次已加载结果查询无新增读取；容量 1/2 的拒绝、释放、复用和 backpressure 均在原有界表中完成。Lua 正常路径计数为读取 3 次、已准备方法槽 23、高水位 23、协程恢复 4 次，结束时 awaitable 和保留结果均为 0。

这些是断言和计数，不是进程 RSS、p99 或提速百分比。原生 query 不执行 IO，不复制全目录；Lua 返回 userdata 的 VM 分配没有宣称为零。保留额度按最大 image/decoded 逻辑收费，provider 索引与临时 codec 分配不算作同一 RSS 上限。完整范围和原日志见 coverage／LastTest。

## G. 未测、免验与历史

| 项目 | 记录 |
|---|---|
| EC2 原生输入 | NOT_RUN_USER_DEFERRED；XEC2-37 为 PARTIAL，后续需单独补测 |
| P12 | 保持 `PARTIAL_USER_WAIVER`；不补回免验，不改判 |
| Linux | NOT_RUN；本轮未建设环境 |
| 系统 IME | NOT_RUN；自动原生输入不能替代输入法实测 |
| Android build | NOT_RUN；仅同步规定 include 前缀 |
| 旧版 50k 深链剩余样本 | PARTIAL_NO_MORE_SAMPLES；不补长测 |
| 历史失败与已修结果 | 按各自实现 SHA 保留，未修改旧 dev_log |

## H. 提交与交付

R1–R8 的 11 个实现提交按责任分批；R9 记录固定同一个实现 SHA。验收材料独立提交到 `dev_log/EC2/`，正常推送实施分支后停在 EC2 复审并等待原生输入补测。不合并 main，不删除分支，不发布 release。

运行已有安装产品可使用 `E:/SyncForder/CodeRepos/install/EC2-dd54847ee281/bin/lux_editor.exe`。只读查看新代码使用上述 `lux-engine-ec2` 工作区；原 `lux-engine` 没有切换提交。Git 逐文件清单保留 156 个路径（新增、修改、删除及重命名），不把文档搬迁或冻结日志算作生产代码功能。

用户原补丁字节 SHA256：`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。保护核验与原工作区状态见 `protection.json`；原补丁归档仍在 `dev_log/P12/protected`。
