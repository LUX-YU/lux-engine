> 最新续行结果见 [Q8-RESULTS.zh-CN.md](Q8-RESULTS.zh-CN.md)，候选e6252e50。以下保留q7历史报告，其身份与结果不改标为q8。

# ER-1 GitHub审阅续行：实际修复与资格

ER-1仍未通过。本轮生产/测试源码候选 `2d2650c5d498d4c74b0ba076dd3f5b322b6c13f7`，已从独立clean clone完成q7正常和专用诊断RelWithDebInfo资格。当前报告提交是独立身份，不重标被编译的代码。未进入ER-2、合并main、推送、发布或冻结。

## 本轮实际修正

- G01：真实失败View在resize/零尺寸/恢复/相同尺寸/camera普通调用后保留FAILED、原View/request/backend错误；明确关闭并重新openView才有新身份。修前exit=1、修后exit=0均正常关闭全部owner；不以timeout判定。
- G02：SceneResources维护粘性pending_change，每个row状态变化在异常退出前仍登记；Session完整发布snapshot/revision后才确认消费。真实失败后准备OOM、READY后另一准备OOM、snapshot自身OOM三组合及无新状态重试通过；下一静止cycle不重发/不复制。
- G03：实际不可变RenderSystem coordinate_page_size经SceneView传至RenderView，用于校验、分页及wire字段。默认1024和256场景统一平移(256,-256,1024)，覆盖负坐标、跨页和非零相机；两姿态真实GPU图像相同。原1024硬编码在256下实际画成背景的负例保留。
- G04：SceneWorkspaceFailure只位于业务UI边界，保留Scene/Window拥有数据；Application按来源转交。真实NotFound及回调BUSY分别贯通同步/关闭；没有向generic WindowFailure注入Scene依赖。
- C06：4个真实View工厂DLL分配点、既定8槽满、第9次接纳拒绝及释放后新身份复验通过。R06新增实际GPU mesh成功后shader元数据被真实后端拒绝，保留backend_status=1和资源key，清理取得的句柄；未据此宣称独立material上传失败已测。

EX-01/02/03、正常/诊断隔离、metadata稳定性和默认相机协议保留。本轮没有modules生产代码修改、依赖升级、完整Impl共享、万能Context、新栈调用旧栈或运行时fallback。

## 实际资格

| 项目 | q7结果 |
|---|---|
| 正常clean | all -j4 -k0、no-op、134/134 CTest；12 Scene GPU变体、foreign、Application生命周期 |
| 专用诊断clean | all、no-op、135/135 CTest；27 Scene GPU变体、foreign、Application生命周期 |
| 分配/预算 | 原seal全部实际分配点包括index=2重跑；原reply budget/FIFO用例保留；不等同资源Control/Upload饱和资格 |
| 安装 | 五类消费者在sdk/relocated-sdk两位置构建、no-op与执行；排除源树/构建树/旧SDK搜索路径；实际loaded modules与安装reader再生成通过 |
| 隔离 | 两种缓存分别OFF/ON，六DLL×两配置的导出、PDB链接对象/CRT分配来源、DLL/PDB SHA256共12记录；正常SDK无replacement allocation/fault路径 |
| 成本 | 五组旧/新独立正常进程、每组500实际完成帧、十PPM字节相同；另64/1024空引用Mesh静止owner各五独立进程 |

源与产物身份：q7/qualification.json、diagnostic-qualification.json（GPU日志由diagnostic-raw原19变体与diagnostic-rerun新增8变体组成）、source-files.sha256.csv、qualified-isolation/dll-isolation.json。原23项source-binding保留起始hash并记录q7实际字节；github-review-binding.csv单列本轮改动与测试hash。

本轮未知engine.zip/modules.zip留在原工作区，没有忽略、移动或混入源码。由候选tracked commit建立独立qualification-input，ValidateTrackedSnapshot通过后再由既有流程建立q7/src clean clone。审阅附件是源码推导；修前/修后由实施方执行，详细日志和无效探索结果见GITHUB-REVIEW-CONTINUATION.zh-CN.md及review-github-01/raw。旧q6原始结果与COST-q6保留。

## 成本边界

500帧主动工作wall均值0.08738692→0.08869704秒；等待cycles均值288817855.8→320411995（五对均增加，约10.94%）；关闭wall均值0.09991118→0.10653356秒。不是整体加速/性能等价结论。

静止owner 64/1024 Mesh的500次更新均值0.00318376/0.1477368秒，缓存snapshot保持同一地址。关联比较由原算法推导2080/524800每cycle；类型payload下界32768/524288字节。此零View空资源引用样本不替代READY资源、大型GPU场景或完整内存归因；见COST-q7.zh-CN.md及逐进程原始JSON。

## 剩余门槛与限制

原118行当前状态：{'PARTIAL': 30, 'PASS_ER1': 58, 'PASS_INTERMEDIATE': 5, 'DEFERRED_STAGE': 24, 'BLOCKED_DELETE_GATE': 1}。逻辑行数与134/135个CTest注册数分别统计；历史PASS_INTERMEDIATE不升级为当前提交GUI通过。

仍未完全通过的原ID：A01, A03, A04, A05, A06, A07, A09, C05, U04, U05, U10, U11, U12, H03, H07, F15, R01, R06, R07, R09, X01, X03, X04, X08, X09, X10, I02, I06, I07, P02, P03。

主要未测路径仍为thread/device/attach分阶段启动失败、预存ResolvedMeshResources冲突、独立material上传失败、分别控制set_stopped/value、真实资源Control/Upload饱和重试、专用shared-import/preserve GPU执行、实际持续RMB/MMB失焦/隐藏/关闭，以及部分关闭借用/值访问器线程审计和resize/retry尾部账目。中文IME候选已实测，其位置/汉字显示存在明确失败。VkDeviceLost专用路径未测，不把record failure改名；被动RenderLease析构中的deferred vector OOM仍无全路径证明。

本提交已执行真实GUI/IME：ni候选与数字选词实际发生，但候选位置和汉字显示有明确失败；普通Alt-F4关闭exit0、descriptor2/2、所有owner0。见IME-CURRENT-FAILURE.zh-CN.md及当前截图。持续RMB/MMB部分仍提供人工步骤，不将旧gui-02换标签。

M26 Toolset冷装配及安装消费者保留通过；M01—M05/M21—M23/M28—M29等删除门槛未满足，旧正式入口仍存在。新候选示例lux_editor_er1在clean build/bin，尚未安装为唯一正式入口；本SDK不是legacy-free。没有先删旧代码换取扫描通过。Text/Record仅为Editing/路由机制，不是材质业务迁移。

本轮交付的是有真实修复和新资格证据、仍保留上述缺口的ER-1续行候选，不能宣称原ER-1所有门槛已完成。

六个受保护main/Script文件及两个未知ZIP的最终SHA256与本轮开始一致，见protected-main-final.json/inputs-final.json。交付归档与CRC/SHA256身份见E:/lux-er1/delivery-2d2650c5/delivery-manifest.json。
