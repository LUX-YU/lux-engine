# P08 R1：临时访问失败保留交互

本轮仅执行 P08 R1，停在 P08 等待复审。前置 `8f210a282109ec9e0dae3f32f88bc04359ef2c65`，
实现 `48ebdb9d8082e391f9c12619ff93934b4ee330c2`。验收从该 SHA 的独立干净检出运行，
显式 `LUX_EDITOR_MIGRATION_STAGE=P08`。`main` 未修改。

`.internal/editor-redesign/` 仍为唯一可变施工材料；本目录为冻结验收快照。
原 P08 与更早的快照按各自 implementation_sha 核验，未改写。复审包、原件及哈希归档于 `spec/`。

## 实际失败与修复

`before/` 是实际已安装 SDK 的九个负例：Scene、Material、Flow 各覆盖 synchronize、cancel 和无手势的选择。
同一真实 SessionStore 按正常 ClosePermit 协议回收 B，其最后 CodeLease owner 析构回调访问仍存活的 A。
回调通过真实 typed access 确认 BUSY；原交互却返回成功，释放临时载荷或清空选择。九次运行均返回 1，
记录完整 stdout、命令、探针源码、SDK 头/库哈希和真实安装链接输入。
作者编码和 SessionInfo 未改变，丢失发生于交互状态；没有把附件隔离头或测试草图当作运行证据。
没有宣称实际 DLL 卸载：普通 shared owner 只用于在真实 Store 回收边界触发回调。

生产变更只在三个 interaction 的 `cancel/synchronize`：

- 删除“任意 read/describe 失败即清理”的分支和误导性注释；只有明确 `STALE_SESSION` 才清理旧身份。
- BUSY、WRONG_THREAD 等访问错误保留原错误类型，返回前不改手势、选择、起始戳、输入所有权或作者状态。
- 正常取消继续经过原 `withRead` gate：将批次移到该作用域的局部对象，先解除活动手势，再销毁输入。
  载荷析构回调看不到半销毁的活动批次，且仍受原 gate 保护。
- 无手势的 cancel 保持幂等；真关闭和新代际清理不借用新会话。

不增加生产类型、公开 API、平行 busy、失效缓存或兼容入口；不改变 SessionStore、SessionState、History、
模型算法、UI、运行、保存和执行器。公共头、目录、生产 target 和包名保持不变。
唯一 Session owner 仍为 SessionStore，交互仅拥有自己的临时批次和选择。

## 五组 R08-R1 与原有行为

新增一份真实三模型回归源，由 native 和安装消费者分别编译；SDK 分支只使用安装的公共头和库。
native 额外检查完整 HistorySnapshot（包括身份、游标、修订、事件序号、内存收费和关闭状态）及 checkpoint。
SDK 通过完整冻结编码、公开身份/绑定/current/observed/dirty/admission、后续提交及 Undo/Redo 验证。

| 组 | 实际检查与归档 |
|---|---|
| R08-R1-01 | `r1-*-sync`、`sdk-r1-*-sync`：真实 B 回收期间返回 BUSY，非空选择/载荷、原地址与 stamp 保持；回收结束后同步并提交恰一条历史，Undo/Redo 恢复编码 |
| R08-R1-02 | `*-cancel`：临时 BUSY 不析构输入；安全点取消后输入仅释放一次，原 gate 仍持有、活动手势已解除，作者完整状态/历史/保存基线不变；重复空取消幂等 |
| R08-R1-03 | `*-selection`：没有手势也必须保留选择并报告 BUSY；正常同步后仍存在，真实领域删除后才剔除 |
| R08-R1-04 | `*-stale`、`r1-*-reload` 和原 interaction 测试：关闭、同槽新 generation、相同 NodeId/UUID、History 重载、内容冲突、删除、持久布局 Undo/Redo；旧交互不能作用于新会话 |
| R08-R1-05 | `*-gate`、`*-thread`：真实模型 withRead 内 cancel/synchronize 拒绝且无副作用；错误线程原样返回；正常清理作用域与析构计数；完整 P08/PLAYER/SDK/UI/依赖回归 |

原 139 项行为和全部既有 C++ 测试/消费者源码保持原样，新增 21 个 native 场景。最终 **160/160**；
PLAYER **11/11**；十二组 SDK 原 **56** 项保留，新增 **18** 项，合计 **74/74**。
八项 Material/Flow operation 复制/按值移动编译负例和真实完成/产物重试继续运行。
不是以数量作为通过依据：门禁核对原测试源码 Git blob、原测试名称集合、具体运行输出及所有负例的失败规则。

原 X08-01～06 的真实 Root/Pane/Element 生命周期、Run 身份和三类交互继续执行；相关 Q 的原范围保持。
实际依赖负例包括原 interaction/view 的 19 项，错误夹具命中原规则，修复后通过，没有放宽边界。
原真实编译、文件 IO 和后端绑定继续验证。核对发现旧资格脚本的 `--fresh` 未传 `CONSUMER_MODE`，
两个名为 GPU/ScenePane 的消费者目录实际运行默认 CPU_UI。旧快照与这些 CPU 结果原样保留；
本轮另以显式 `GPU_UI`、`EDITOR_SCENE_PANE` 配置同一安装消费者，完整构建、二次无工作及运行真实渲染测试，
证据为 `explicit-*-*.log`（上述 74 项之外的两项模式验证）。不把 CPU_UI 算作 GPU 通过，
也不将这些旧产品回归扩大为 P10/P13 新产品 GPU、像素或 IME 资格。

## 验收与保留事项

- Editor 和 PLAYER 使用 `all -j 4 -- -k 0`，第二轮无新增工作。构建、CTest、SDK、GPU 按序运行。
- SDK 重装并重建十二组消费者；未修改 modules 公共头，仍沿既有核验脚本同步并校验原五头的三前缀一致性。
  不执行 Android 构建。
- `check_receipt.py` 只读归档文件和固定 Git blob。原生产机器绝对路径只是命令记录；生产目录不可取得仍通过，
  缺失或损坏归档明确失败。`artifacts.json`、`files.json` 和 `FILES.md` 提供哈希与实现文件清单。
- C01 仍 FAIL，责任 P09/P12；C03 仍 FAIL，责任 P11；C04 仍 FAIL，责任 P12。原断言未改，本轮再现并保留输出。
- 原冷构建失败和 P06 修复证据保留。本轮为干净源码、复用现有构建树，不声称全新冷构建通过。
- 开发期测试曾错误地仅认识 SESSION/BUSY，未识别 Scene 已有领域 BUSY；已修正测试错误分类，模型错误契约未改。
  这与 before 中真实原生产错误分开记录。
- `ProjectBuilder.cpp` 原差异 SHA256 保持 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，
  不纳入提交。旧 rooted 构造与旧 UI 适配仍只服务原登记消费者，最迟 P12 删除；本轮未新增桥。

实现与验收分别提交并正常推送 `codex/editor-redesign-v4`，不进入 P09。
