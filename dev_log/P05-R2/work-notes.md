# P05 R2 施工记录

前置验收 5a1ccbb47d2d08ffa8cfda907f74319c4a694068，实施分支 codex/editor-redesign-v4。
实现提交 3a75bfe6f743d8b9bb74f93f89219054aee2119d。只执行 P05 R2，不进入 P06。

## 修复前证据

先仅增加真实 SDK 消费者测试，链接已安装 R1 静态库和运行库；生产源码未改时运行。
MaterialSaveSource 包装器、SaveExecution、ExecutionRuntime/stdexec CPU scheduler 均为实际实现。
W1 物理发布未采用，W2 编码 TaskInfo.finished 后，W1 accept 首次 collect。
成功、MaterialCodec 类型化 INVALID_ARGUMENT、已有 stop 机制取消三次均退出 20：
collected=1 / encoding_calls=1 / stuck_encoding=1 / reserved=1 / follower_ready=1 /
follower_blocked=1 / final_disk=material1。取消不能释放 W2。主动退出避免析构 terminate 混淆。
before 保存测试源码/补丁、SDK 链接命令/库哈希、三个运行日志、返回值和实际文件。

## 唯一完成路径与保护

采用补充规范的方案 A。已接受的 ENCODING 工作在 owner 上可靠吸收完成事实。
不新增暂存队列、不重跑 encoder、不调用 ISaveSource、不销毁 rebind 或删除 Operation。
只释放原操作的 snapshot allowance，并向原 ticket 提交字节或结构化失败/取消。
OwnedEncodeJob 在 takeEncoding 时已转交任务，收集结束后由任务销毁；code lease 顺序保持。
DispatchScope 用 previous 恢复保护，内层完成不能清除外层 callback 的 dispatch。
新请求、取消、编码调度与确认在 callback 内仍 BUSY；递归 adopt 仍推迟；status/撤销仍可用。
SaveExecution 不再无条件忽略完成交付返回值；正常完成无 dispatch BUSY，只有错误 owner、
失效 ID 或重复交付这种违反 adapter 生命周期/恰好一次契约的情况触发 release 可见失败。
未将终止作为正常 BUSY 的处理。三类模型、协调器和执行器本身均未修改。

## 回归与范围

新增五个 CTest 入口覆盖 R05-R2-01～05：真实 CPU 成功/错误/取消、stop+join、
describe/capture 的正常/异常/撤销六种组合各 12 次（72 次）有界循环、task_capacity 拒绝。
每次验证两份快照容量恢复及第三份拒绝；原 job 编码一次/析构一次、没有遗留票据。
成功分支读取 W2 冻结文件值和 Undo 保存基线，再放行 W3；递归采用/确认仍被拒绝。
旧 111 个测试及原断言保留；原 models.cpp 每一行仍按顺序存在，其他旧测试源码不变。
研发中测试曾误取 TaskInfo 历史任务，改为等待最新 submitted；相应失败日志保留。
新增 task-capacity 测试曾使用不存在的 Task.wait，改用实际 TaskInfo/collect/dispatch；编译失败保留。

旧 P05/P05-R1 验收按各自 implementation_sha 验证，不覆盖历史快照。
C01(P09/P12)、C03(P11)、C04(P12)保持原 FAIL。本轮不改判定、延后新问题或引入兼容桥。
原 Physics2DDescription 冷构建顺序失败继续引用 P05-R1 原日志；独立风险未修复。
资格复用独立干净 clone/build，重新 checkout 当前实现 SHA、显式 P05、全量 all/-j4/-k0、
二次无工作、完整 CTest、SDK 重装、九组消费者与实际依赖负例。不是全新冷构建首轮通过声明。
ProjectBuilder.cpp 原用户差异哈希 ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c
保持不变，不加入提交，也不加入独立检出资格。

归档门禁首次把 Ninja 的 @response 文件命令误当已展开的库名列表，明确拒绝验收。
原修复前命令、SDK 哈希和日志不修改；补存最终安装消费者 build.ninja 的实际链接规则，
其链接声明本轮未改。该扩展规则在 SDK 重装后取得，不能冒称修复前的 response 文件副本。
门禁同时核对原命令、原 SDK 哈希清单、安装包链接声明和实际展开规则；失败日志保留。
