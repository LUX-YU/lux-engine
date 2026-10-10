# R3 — Logical RenderGraph 工作单

用户在 R2-FIX 独立审查中正式批准本阶段。入场读取实际 origin/codex/render-v2：
`e0eabe1640d94fafb80a57bd334507162f3b2f9f`，与批准基线一致。
前阶段 implementation=`4ae55059cca949178a8a1f2eaffe32f720384e8d`；
verification=`e0eabe1640d94fafb80a57bd334507162f3b2f9f`，验收提交只新增原报告。
本工作单记录用户已有授权，不改写 00/11 的历史状态以自行取得授权。

## 范围

- ALLOWED：`modules/function/render/graph/**`、bootstrap 必要接线、`docs/render-v2/R3_*`。
- READ-ONLY：已验收 Core/Transport、`render_legacy/**`、既有架构规范及历史验收报告。
- FORBIDDEN：Vulkan、Runtime、Scene/ECS、Editor、Feature execution、FrameLoop、Root/产品接线。
- 原用户 checkout 六处修改全部隔离；继续使用现有 `codex/render-v2` worktree。

## 输入/输出合同

Definition 拥有 backend-neutral 资源、pass、用途与显式依赖。编译返回拥有定义快照、
确定性顺序、资源依赖和逻辑使用区间的完整 immutable logical plan。FrameBindings 借用
指定 plan 和导入绑定，仅承载当前帧事实，不编译图、不接收 Simulation tick。
当前没有生产 Graph consumer；不为了未来 API 引入 GPU/业务 schema、执行 callback、
通用时钟、缓存 Manager 或插值系统。

采用 V1 的显式依赖优先排序、资源使用索引、Kahn 排序和使用区间不变量；
保留适用的逻辑测试向量，见 Graph README 的逐项映射。所有循环和非法引用返回 expected。
补齐整资源 WAR 边；不迁回 painter stage、local-read glue 或 Vulkan owner hierarchy。

相同规范化拓扑按精确值比较可复用同一 plan；frame serial/time/slot、backing、offset 不参与
拓扑比较。真正资源/pass/use/dependency 变化由 caller 在冷路径编译新候选，失败保留旧 plan。
不用 hash 代替拓扑相等证明，也不要求每帧检查或重编译。

## 验收

1. 实现提交后运行 ValidateTrackedSnapshot，从精确提交建立独立 clean clone。
2. 完整 bootstrap MSVC x64/Ninja RelWithDebInfo `all -j 4 -- -k 0` 两轮；第二轮无工作。
3. 保留 27 个旧 CTest，新增声明/依赖/环/RAW-WAW-WAR/producer/使用区间/帧绑定/复用/
   candidate rollback、确定性生成图 oracle、独立公共头和依赖编译负例。
4. 检查实际 File API/source/include/link/codegen 闭包，不链接 Transport 或禁止层。
5. 单独记录一百万次 frame binding 的 C++ 分配与批次 p50/p95/max；这不是 FrameLoop/GPU 性能。
6. 核对 719 个 legacy 文件、Core/Transport、旧文档、用户修改保持原样。
7. 公共头同步三个安装 include 前缀，资格构建只消费独立的 generic dependency prefix。
8. 输出源码外证据及 manifest；verification commit 只新增结果报告；提交、推送后 STOP。

产品仍为 EXPECTED_UNAVAILABLE。GPU、完整产品、V1 全矩阵、installed SDK、Android、Linux、
Sanitizer 和最终渲染性能本阶段 NOT_RUN。R4 需要用户新的明确审查授权。
R5 继续保留 ticket consume/abandon 责任封装与独立异步诊断事件的待办，不在 R3 补做。
