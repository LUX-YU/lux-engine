# P08 R1 施工记录

仅执行 P08 R1。当前前置 8f210a282109ec9e0dae3f32f88bc04359ef2c65 是已推送 P08 验收提交，
833d7efb18f8349cda9968ac3e4ea24ff2b54c60 为其祖先；实施分支 codex/editor-redesign-v4。
main 仍为 2bb33ff1a1f11025cf404e074c0e9b259239d8a4。
ProjectBuilder.cpp 既有工作区 SHA256 ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c 保持。

复审包 MANIFEST 各文件哈希核对通过。复审包 probes 是隔离草图，没有加入生产或验收目标。
唯一可变账本为本目录 architecture/migration-ledger/test-coverage.json 的 p08_r1 项；P08 原记录不改。

## 修复前

P08-R1-before 保存真实已安装公共头、静态库哈希、编译及链接参数和探针源。
Scene/Material/Flow × sync/cancel/selection 九次运行均以原目标断言返回 1。
真实 B 的最后 CodeLease 析构回调中实际 Store 查询 A 返回 BUSY，A 仍存活；原交互却成功清理。
完整作者编码和公开 SessionInfo 保持，overlay/selection/起始 stamp/输入寿命检查失败。
没有修改 Store 私有状态，没有加载/卸载真实 DLL，也没有用 WILL_FAIL 把错误判为通过。

## 实现

只改三份 interaction cpp 的错误分类和原 gate 内输入销毁顺序；不新增生产类型、公开接口或 owner。
cancel 的非 STALE_SESSION Store 错误原样返回；synchronize 同样先分类 describe 错误。
活动输入移到清理作用域内局部 batch，再 reset optional；本地 batch 在原 withRead 返回前销毁。
明确 stale 身份不借用新代际，不把临时访问失败缓存为失效。

原 139 项测试源和断言逐字不改。新增同一真实回归源供 native 和安装 SDK 各自编译，
native 额外核对完整 HistorySnapshot/checkpoint，并通过私有原 PreparedReload 检查三类重载身份。
SDK 分支只包含真实公开头，不借用模型私有访问。

开发期第一次扩展 gate 测试把 Scene 原领域 BUSY 错误误当成 Material/Flow 的 SESSION/BUSY；
三个 Scene 测试因此未通过。修正测试分类以同时检查 Scene 的真实领域错误和 Store 错误，
没有更改模型错误返回或 gate 算法。随后新增 21 场景通过；原九个修复前失败证据不变。

## 验收安排

实现 48ebdb9d8082e391f9c12619ff93934b4ee330c2 单独提交。
从该 SHA 的独立干净检出显式 P08 配置执行 all -j 4 -- -k 0、第二轮无工作、完整 CTest、
SDK 重装及十二组消费者、八项编译负例、PLAYER、真实 UI/GPU 和所有原依赖负例。
原 C01/C03/C04 保留 FAIL 与后续责任，原冷构建证据不变。本轮不声称全新冷构建或 P10/P13 产品资格。
最终冻结 dev_log/P08-R1，单独提交证据并正常推送，不进入 P09。

验收日志核对发现旧消费者 --fresh 没有保留 CONSUMER_MODE，GPU/ScenePane 命名目录实际跑 CPU_UI。
原记录不改，本轮保留原 74 项并追加显式 GPU_UI / EDITOR_SCENE_PANE 模式资格；不修改生产范围。
