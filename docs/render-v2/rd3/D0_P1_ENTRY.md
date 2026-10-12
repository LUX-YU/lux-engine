# RD3-P1 入场建议 — 未授权、未执行

D0 机械完整性可验证，但整体设计冻结为 PARTIAL。建议先审查 R01–R04 和 R05 说明、批准 RD3
权威替换及修订阶段映射，然后明确授权 P1。不可将此次 D0 请求当作 P1 开工许可。
P1 本身无需先实现 GPU Asset、完整 Effect Compiler 或新 Vulkan Executor；它也不能宣称解决后续完整 Target 事务。

## 最小生产改动建议

1. 从 `runtime/src/Runtime.cpp` 的 Impl/Builder 中拆出现有 Backend/Transport/worker 装配责任。
   Client 只保留协议提交/结果/诊断；应用或 Server bootstrap 持实际 Native factory 和生命周期。
   继续用 R2 队列、Reply arena 和一个 owner-thread，不增 EventBus、服务容器或第二 worker。
2. 把 `runtime/pinclude/RuntimeBackend.hpp` 中 Scene/View 的语义状态实际迁入中立 Scene/View；
   将全局 owning View 表改为 Scene 内 owning SlotMap。确立含 owner/generation 的身份，跨 Scene handle 拒绝，
   全局按 Graph 分组的索引不拥有 View。Native import/receipt/slot 保留在后端执行责任中，不能复制为中立 View 字段。
3. 中立 Target/描述/Layer 保存最终输出语义。现有 WSI/fence/资源销毁算法先留在 Native 实现；
   P1 不重写 clear/blit/Graph 编译；P3 负责把需要的最终合成纳入完整 Graph，不把过渡状态称终态。
4. 必要的 EffectId/GraphId/ResourceDomainId 强身份及协议字段边界可以在 P1 定义；
   P2/P4 实现其真正对象。禁止创建空 EffectManager/ResourceRegistry 壳或再建 CPU ECS 数据镜像。
5. 只按真实 include/link/install DAG 调整 Runtime、新中立对象、bootstrap 和对应消费者/测试的 CMake。
   建议触点见 `D0_MIGRATION_MAP.csv` 的 P1 行；精确 ALLOWED 文件列表必须在获授权后核查当时 HEAD 再冻结。

生产核验范围：R2 Reply/Admission/stop/drain，Client/Request 晚析构，Builder 线程创建失败回滚，
Scene owns View/代际/跨 owner 拒绝，2 Scene/4 View、窗口/offscreen、多 Target、无 PROGRAM 独立帧节奏、
diagnostic 与 reply 分离、Device Lost。保留现有 GPU fixtures、全量 all -j 4、必要第二轮 no-work、
Normal/ASan/SyncValidation、安装后公共头无 Native 依赖和受影响性能合同。
不因改目录重做已正确的 C0–C13、Queue/WSI/Shader ABI 算法；变化影响到哪些合同就重验哪些真实消费者。

当前机器开发环境已在前序任务配置；该开发树 192/200 的结果（8 项 GPU/Runtime 失败）不是本次 D0
独立资格，也不能继承为 P1 PASS。SDK 版本与历史环境不同；没有足够证据把差异认定为失败根因。
P1 开工前需要先明确这些现有失败的可复现基线，再冻结影响范围；本轮没有改生产代码来绕过它们。

未来 P1 仍采用新的 I → 精确 clean clone V → 推送 → STOP。此文件只提出建议。
