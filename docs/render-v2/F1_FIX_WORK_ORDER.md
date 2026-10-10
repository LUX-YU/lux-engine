# F1-FIX — 作者契约修复工作单

授权仅覆盖 F1-FIX；基线 `877d44c6eaea03dd5746b03f67af158e97a8ee71`。
F1 实现为 `78a1248d1f5e9c59eb8f88ddd4869244e512050f`，Frozen V1 为
`a669409a289a6fa4092f21176397795b1cdb7f3e`。开始前已通过 `git ls-remote` 核验实际远端。

## 目标与范围

A. Graph 使用唯一中立 `rdesc::ETextureFormat`，显式分类与逻辑 usage 验证。
B. 有限、可验证的 Shader buffer/image view 访问范围，同源 Schema 与 Definition binding 一致。
C. 同一 Params 的 Vertex + Fragment 声明、各 Stage 反射与完整程序校验；保留 Legacy 输入头部顺序。
D. 有规范名称的资源/Pass/Shader asset-variant 作者身份，不依赖作者手工分配数字。

允许修改 `modules/function/render/graph/**`、`engine/toolchain/shader/**`、中立
`modules/resource/description/**`、必要 bootstrap 测试接线和 `docs/render-v2/F1_FIX_*`。
Core、Transport、Vulkan Foundation、719 Legacy 文件、FINAL 包和既有验收报告全部只读。
不新增 Meta Parser、Provider resolver、完整 Hazard Scheduler、Runtime 或 Native Graph。

## 实施与验收

在现有 V2 worktree 施工；保护原用户工作区的 HEAD、状态、diff 和六处原始字节。
保留全部原 83 项 CTest，以及 R3 排序、随机图、生命周期、10k bindings/计划复用 oracle。
新增范围/格式/命名测试、Stage Shader 生成/反射/联合合同、原样 Legacy Shader 输入及半浮点存储 fixture。

提交 implementation I 后，从 I 建立 `clone --no-hardlinks --no-checkout` 独立检出。
tracked snapshot → configure → 全量 all -j 4 -- -k 0 两轮 → 全部 CTest → CPU ASan 与完整 ASan 重试
→ 真实 source/include/link/codegen 闭包 → Graph binding before/after → 保护复核。
所有输出保存源码树外。验证发现生产问题时产生新 I，重新独立验收，不在 clean clone 修补。

完整 ASan 使用同版本 SPIRV-Cross 的隔离 `/fsanitize=address` 构建，保留 STL annotations，
不修改第三方源码/正式安装包/ABI 定义；失败时如实 PARTIAL，不以禁用检查换绿。
三个模块公共头安装前缀同步另存备份、源哈希及结果；不代表 Android 或 installed SDK 资格。

仅新增验收记录提交 V，推送后 STOP；F2 需用户复审及新的明确授权。
产品仍为 `EXPECTED_UNAVAILABLE`。
