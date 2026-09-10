# V4-H1 实施结果


实际交付保留 H1 Native prepared step 去重和 H3 checked release 收敛。H2 已编码、验证并撤回，原因是实测 FlowForge Event 回退。

`implementation=PARTIAL`；`contract=PASS`；`cost=NO_CLEAR_GAIN`。不宣称全路径提速或性能等价。

## 身份


固定 V4 `f7d2815bdd2025ee23a7c11449def822413f58e9`；撤回前候选 `655855401e09aac2d4e8becd16afca62086291ee`；**最终资格与计时源码 `7c565a0014d7c82f0d522eb4ca8c8f0f64773893`**。后续文档/证据提交不是新测量身份。

分支 `codex/s6-v4-prepared-checks`；V4 分支 f92853ea、A1 分支1039f2bc 保留；main 的7个未知文件及原始镜像哈希不变。独立 clean clone 构建，独立 `install/o/v4-h1/{sdk,tools}`。

Lua5.5.1 + lux-lua55-v3-r2、INC、16MiB、Native ABI6、lux-cxx3100f54d、toolset99c3d048、MSVC19.44.35228 /O2 /Ob1 /MD 与 V4 相同。三份 cooked fixture 字节相同。详见 result.json 和归档 final-audit.json。

## 实际变化与未采用项


| 项目 | 实施/结论 |
|---|---|
| H0 | 五份 V4 软件热点、调用栈和 A/B PDB 指令片段；只有全目标进程范围，没有精确循环ROI/PMU。 |
| H1 / 9ecc5432 | checked入口定位step一次；create内核不重复非零size/对齐/上界；slot、frame、lease、packet、outcome及销毁顺序保留。 |
| H2 / f7f7b1c1 → 7c565a00撤回 | Event prepared/raw gateway共用动态内核曾完整落地；custom ABI残余检查保留。最终 ScriptExecution.hpp 恢复V4，新负例继续保留。 |
| H3 / c85c40f1 | Ticket与Allocation各自验证后进入private releaseResolved；不合成Allocation再验证自身。完整代次/owner/data-size错误仍拒绝。 |
| Native固定除法 | NOT_SELECTED_METADATA_TRADEOFF：实际div仍在；缓存商使Class32B→40B，目录按prepared容量增长，未采用。 |
| H4 | ALREADY_BOUNDARY_ONLY：Lua current一次prepared解析，revalidate无目录重复；默认scalar can_reenter优化是V4既有成果。Lua生产源码未改。 |

检查逐谓词的证明、失效点、原拒绝入口、源行及机器码见 [CHECK_AUDIT.csv](CHECK_AUDIT.csv)。实际断言和覆盖范围见 [TEST_MATRIX.csv](TEST_MATRIX.csv)。

## H2 的负结果


65585540 对 V4 的 FlowForge Event 三对 +2.765% / +0.532% / +5.951%。唯一一次有限撤销诊断只换回 V4 ScriptExecution.hpp，保留其余候选DLL：三对 -4.386% / -4.023% / -4.163%，业务完全相同。因而撤回H2，不尝试 forceinline 或第二轮消融调参。

机器码支持具体代价：H2 把完整 reserve 内核变为额外调用目标；waitEvent体积1080B→1228B。只能把整项改动视为已定位成本，不能将13ns全部精确分摊给某个分支。撤回前42个有效进程、6个撤销对照均保留。撤回改变了最终源码，故另行绑定最终构建/安装及下列42进程；不复用旧数据重标身份。

## 最终七腿同量对照


A为固定V4，B为最终7c565a00；各3对AB/BA/AB。变化为 `(B/A-1)`，负值更快；“配对中位”不是两侧中位数相除。

| 场景 / 单位 | 三对 A → B | 每对变化 | 配对中位 |
|---|---|---|---:|
| event / ns/完整调用或等待周期 | 657.845→649.022；664.645→652.561；725.140→717.605 | -1.34% / -1.82% / -1.04% | -1.34% |
| nextstep / ns/完整调用或等待周期 | 747.557→704.483；727.574→728.549；731.109→730.520 | -5.76% / +0.13% / -0.08% | -0.08% |
| flowforge-event / ns/完整调用或等待周期 | 312.416→311.581；312.326→316.183；310.631→318.320 | -0.27% / +1.23% / +2.48% | +1.23% |
| cpp-sequence / ms/完整帧 | 2.612→2.642；2.594→2.666；2.628→2.626 | +1.16% / +2.77% / -0.07% | +1.16% |
| scalar / ns/完整调用或等待周期 | 144.227→139.975；141.901→139.831；143.752→140.730 | -2.95% / -1.46% / -2.10% | -2.10% |
| record / ns/完整调用或等待周期 | 1299.312→1310.423；1303.044→1319.307；1309.700→1320.829 | +0.86% / +1.25% / +0.85% | +0.86% |
| physics / ms/完整帧 | 2.786→2.759；2.775→2.764；2.790→2.746 | -0.95% / -0.41% / -1.57% | -0.95% |

正式负载不变：Lua三腿10000实例×2000帧，预热1000；FlowForge Event10000×1000帧，预热300；ValuePose100万双向调用，预热1000；C++ Sequence原1000完整帧，未额外drain；Physics原2000完整帧、预热300、workers0。每条CSV业务列逐行A/B相等，所有进程exit0。

批量总ns、p50/p95/p99/max帧、累计calls/resumes/suspensions、错误与backlog见result.json.runs及原CSV。它们是帧分布，不是逐resume延迟测量。ValuePose只有批量计时，没有伪造逐调用p99。C++ Sequence未暴露累计失败计数，保留null；其它腿的错误观测位于计时区间外。VM统计关闭为null，不是0。

## 资源和机器码


最终A/B sizeof与选定owner backing逐项相同：PreparedInvocation32、C72、A192、Ready24、EventWaiter88、TimerWait104、BoundedClassStorage208、Ticket24、Native Class32。逻辑N=10000，物理A=10240；选定四owner与N方法/单endpoint的动态存储加对象共 **9,434,554B**。该口径含诊断分配header/alignment，不是引擎总内存、RSS或Lua VM统计。详见layout-final2原始输出。

未新增成员、证书、cell、结果池或身份目录。Native create机器码228→187B，分支指令10→4；invoke1035→993B。C++ Ticket186→165B，Allocation410→198B，新增共享内核195B；Native/Lua相同模板的内联结果不同。分支数含跳转，不能据此推断预测失败率。Lua current548B/revalidate215B保持。每个DLL/PDB/RVA见machine索引，实际文件大小见final-audit.json。

## 验证与原始记录


最终Developer **126/126**，Toolchain **110/110**；两轮target all -j4 -- -k0与no-work检查通过。固定VM4/4；当前SDK15/15、增量13类、迁址2/2真实运行。迁址时原源码/构建/SDK/VM路径隐藏，随后全部恢复。

新增并重放：释放prepared入口拒绝；Event source/A双满；4种custom payload原阶段拒绝；Ticket两种统计模式坏释放；Native冷描述8种×A/B；Native实际packet六种错误和ABI/outcome八场景×A/B。native-boundary探针中provider/backend resume计数、frame和lease释放均实测。

基线与65585540的8条新轨迹相同；最终CTest保留全部新增断言。另以最终DLL重放容量1的pin内取消/嵌套登记：activeA已0仍因物理槽被pin占据而拒绝新等待；两侧关闭后均清零。机器码、布局和SDK均绑定最终7c565a00。完整路径和错误范围在矩阵中展开，不用测试名相似替代断言。

原始日志只归档一份：[证据入口](../evidence/script/script-v4-h1/INDEX.md)。本机原始目录 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-v4-h1`；原VTune result可直接打开。诊断源/命令/哈希随归档交付。

## 失败、限制与停止点


保留首次style失败、报表参数失败、诊断编译/配置修正日志。Native诊断的无默认构造编译失败和max_ability_imports=0冷拒绝属于驱动问题，没有调整生产拒绝行为。外层PowerShell还输出一次input-line-too-long警告，具体初始化命令未隔离；各构建/运行目标退出码和业务oracle独立通过，警告原文保留。H2是有效负结果，未按“异常慢样本”删除。

未解决V4 scalar成本和Native metadata债务；未改VM/GC/allocator/ISA/IPO/原资产。没有PMU、逐恢复延迟或新增RSS观测；没有重测Lua54/JIT或Android。既有MSVC深层public expected包装器限制仍在。

建议只审阅最终保留的H1/H3及测试；H2本轮收口为未采用，整体实施标PARTIAL。成本结论只对实际分布成立，不授予性能等价，也不把未解释差额归为必要安全成本。停止于本分支，未合并V4/main、未发tag，等待独立审阅。
