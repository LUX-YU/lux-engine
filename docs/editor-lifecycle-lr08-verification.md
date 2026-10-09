# Lifecycle LR08：全清单补正与最终资格

实现固定为 `f15ff04175d6b6e52180903b0505771b9e32aeea`，lux-cxx 为
`0a0e7419fc7229df6e372cd35a540249f92250ef`。本阶段整体 **PARTIAL**：Linux 按用户决定未执行、未通过；
既有最小化问题未关闭，不以新的有限通过记录覆盖历史失败。最终 Windows ASan 已完成；其覆盖限制单独列出。

## 全清单与补正

逐项保留 LR00 的 407 条责任记录、127 个原文件路径；包含此前易遗漏的 63 条 LR02/LR03 组合责任。
原阶段审阅与证据按自己的 implementation SHA 核验，当前源码及最终测试另外绑定本次 SHA。
逐项审阅是依据，正则清零只作附加检查；保留 native leaf release，不把真实 GPU/事务状态错误地删成空壳。

| 原问题 | 实际唯一 owner 与迁移 | 修前及修后依据 |
|---|---|---|
| TransientVertexRing 与 triangle overlay | 现有 VmaBuffer 拥有槽内分配；完整替换候选，原 DeferredDestroyQueue 退休；三种 feature 使用原 frame slot | 真实修前 native 失败泄漏及丢失旧 backing；六种分配失败与三个实际 feature 安装/卸载；删除 destroy、destroySlotBuffers 和私有帧计数 |
| UI sampler | 原 SamplerOwner 与完整字体资源候选 | 真实字体失败、重试、八次安装/卸载和 Scene 清理；删除手工 sampler 释放 |
| Render graph allocation cache | cache 的 VmaImage/VmaBuffer 拥有 CURRENT 分配，PREVIOUS/imported/external 保持借用 | 原 CURRENT ping-pong 真实泄漏 1/3/3；失败前缀回滚、GC、借用图像和相对尺寸；删除聚合 raw destroy 原体 |
| 同步 transfer facade | 零真实消费者的 createTexture2D/flushPendingGpuTransfers 整体删除 | 实际消费者、公开声明、DLL 导出与安装闭包核对；保留 compileShader 和正式 accepted upload 路径；不虚构修前运行失败 |
| MeshShadow | CullBuffers/MappedBuffer 内现有 VmaBuffer，替换先准备后采用，原 serial queue 接收退休责任 | 修前 cull 分配拒绝仍安装、detach 立即释放均真实失败；六步安装失败、三步映射/替换失败、增长/flush 失败重试和精确 serial 回收 |
| Physics2D scatter | 在 ECS patch 前将位置和速度求值为两个拥有型 Vector2d | ASan 真实 stack-use-after-scope：auto 保留的 Eigen 表达式引用已销毁的临时 Vector2f；增加大坐标原点、非零 XY 速度、连续步进和有限值检查 |
| Compiler-result 夹具 ABI | 夹具与 DLL 使用一致的 NDEBUG 配置；27 条原表达式改由始终有效的 require 检查 | ASan 真实 new-delete-type-mismatch：夹具分配 400 字节，DLL 析构按 312 字节；修正测试构建而不修改 Renderer 布局，不抑制 ASan 检查 |

MeshShadow 夹具使用实际生产 feature、Vulkan 分配和原退休队列，验证 engine frame slot；不宣称该夹具提交了
阻塞 GPU draw。完整实际呈现由已有 GPU 回归另行验证。没有新增管理器、资源队列或第二退休算法。
其他 LR01–LR07 完成的迁移不重做；ArenaAllocator/GPUBufferVma 的 native leaf 析构是正式边界，明确保留。

## 最终固定提交验证

独立 clean tracked 检出 `D:/LuxQualification/lr08-final-source` 先通过 ValidateTrackedSnapshot；
构建与 GPU 串行，所有 all 均为 `-j 4 -- -k 0`。
六处用户差异不进入该检出；SDK 消费者只使用新安装头、库和 DLL。

| 验证 | 已取得结果 |
|---|---|
| Editor 首轮 all / 第二轮 / CTest | 1250 步通过 / no work / 121/121，含 40 个 GPU、8 个 desktop 标签项 |
| PLAYER all / 第二轮 / CTest | 1153 步通过 / no work / 58/58 |
| 新 SDK 原消费者 | 192 步 all / no work / 16/16 |
| 公共头与实际消费者 | 160 步 all / no work / 10/10；LR 全程 38 个已改公共头独立 C++20/noRTTI 编译 |
| 最小安装消费者 | Window 1/1、Physics2D 1/1、Script 2/2；各自 all/no work |
| source/include/link/install 闭包 | Editor 661、PLAYER 605、原 SDK 59、公共消费者 50、三个最小消费者 1/1/2 个 TU |
| 实际依赖负例 | Object→Process、bridge→Editor、bridge→UI，均合法通过→指定规则拒绝→去边恢复 |
| 三安装 include 前缀 | 26 个 modules 公共头逐字节一致；Android 仅同步头 |
| ASan | 独立 clean all / no work / 121/121；220 个实际第一方 target、661 个 TU 检查到地址插桩与一致的兼容配置 |
| 可搬迁归档及缺失/篡改拒绝 | 中文/空格路径通过；删除或篡改真实 SDK 输出均被拒绝 |

依赖负例第一次脚本预期链接边标记，但实际导入的 Process include 被更早拒绝；保留输出和原脚本，
按真实检查顺序要求精确 INCLUDE 标记，生产检查器未放宽。前缀精确核验发现九份副本仅换行符不同，
保存原字节后同步；不存在用旧 SDK 补全新安装的情形。

ASan 首轮保留 C1128 目标文件节数上限以及 SPIRV-Cross/LLVM/MLIR 的 STL annotation 链接冲突。
独立资格配置使用 `/bigobj`，统一关闭 string/vector 容器溢出 annotation；第一方地址插桩仍启用。
这项覆盖不包含预编译第三方内部、STL 容器逻辑边界、UBSan 或 Linux；不关闭运行期 ASan 诊断。

## 证据与剩余范围

外部材料：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr08/`。
本次修后最终资格位于其 `final/`；较早 `75ef91ae8` 的普通测试通过与 ASan 119/121 失败保留为历史，
不会被修后记录覆盖，也不算作新提交的执行成绩。
最终 `verified-evidence/` 冻结 11331 个文件、189 条命令、37 条最终必需命令。
Manifest SHA256：`16629af47fe93031f140b6cf4d0e5d88c910fe7ae1d83eb93b217cd759920bc2`。
验证输出为 `runs/logs/lr08-final-qualified-verify.log`；验证只读取归档内的相对路径，缺失时不回退生产路径。
修前产品失败、夹具错误、构建错误分别保存，不混为一类。

Linux NOT_RUN/未通过；原生输入 NOT_RUN_USER_DEFERRED，系统 IME 未测。`Q-LR03-HOST-MINIMIZE` OPEN，
原 skinned transient pool 跨帧 WAR 责任不变。有限 CTest 通过不等于根因关闭；不补旧 50k 性能长测。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。六处用户修改保持原哈希，
ProjectBuilder 独立补丁未应用，main、历史快照与免验结论不变。MA 启动是否允许带着 LR08 PARTIAL 推进，
需按用户明确答复记录，不能把 Linux 未通过改写成满足 LR08 PASS 前置。

自动审批拒绝了清理两处旧临时构建目录，原返回仅为 `blocked by policy`。未执行删除，也未换途径绕过；
它们仍保留，不影响本次源码和安装结果。
