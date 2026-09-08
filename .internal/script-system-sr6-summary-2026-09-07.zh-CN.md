# SR-6 联合候选：一页审阅摘要

2026-09-08当前审阅状态：**本轮限定范围验收通过；性能等价未授予；残余成本保留。**
依据用户本日验收决定，关闭安装SDK真实生成Ability→provider→结果→关闭执行的覆盖项，
结束本轮SR-1～SR-6结构重构与首批值转换实施。已量化成本及未解释份额作为非阻塞后续事项保留。
审阅基于`177b110e8610e7071bba4454c3143b6bf2b8f0b1`的源码与交付记录；审阅者未独立复跑或解包校验归档。
本次仅登记文档并检查链接、路径和diff，没有新的生产/模板变化、测试运行或归档重打包。

2026-09-08完成。资格源码F=`7b5e1dd4f824e0a2746b1dbb0ec2d0dcc57fe490`，入口生产E=`f64cadde`；
lux-cxx=`3100f54d`、toolset=`99c3d048`保持。生产运行时与生成模板E/F无差异；后续提交为报告与证据，不重标二进制。

本轮补齐已安装的`script-lua-values`真实纵向链：consumer自有头→安装生成器→Lua table→生成Ability→
真实provider→非恒等字段结果→关闭回收。通过公开Simulation/ScriptSystem与合法Hook调用，无Engine私有测试入口。
两个错误输入各自provider增量0，随后合法恢复；普通合法2次、Begin/End各1次，provider构造/析构与lease/release均1/1。
在新SDK位置重新生成/编译/执行，原候选源码/build/SDK临时移开、搜索路径隔离后仍通过，已恢复所有目录。

| 资格 | 结果 |
|---|---|
| IMPLEMENTATION | PASS：消费者/驱动实际实现；七owner及C01—C13有限复核完成，无需新增生产补丁 |
| CORRECTNESS | PASS：Toolchain全量108/108、Developer全量123/123、Lua54受影响子集110/110；原六项Scene Lua已通过 |
| CODEGEN_INSTALL | PASS：15消费者、13增量检查、纵向relocation、all构建与第二次no-op；私有头和依赖闭包检查通过 |
| PERFORMANCE_EVIDENCE | 同量记录交付获认可；没有统一性能PASS或等价结论，旧scalar/coroutine/record/Event债务保留 |
| EXTRA_PLATFORMS | 未验证Linux/Android/额外配置/完整Editor、Lua54全profile与全部消费者 |
| OVERALL | 本轮限定功能、Windows RelWithDebInfo指定配置的有限工程验收结束；成本和未验证环境保留 |

E→F八场景各五对独立进程、80次运行，完整工作量和checksum相同；每场景的五对变化均有正有负。
scalar配对中位−0.148%（−0.125ns/provider），coroutine−0.159%（−1.684ns/实际新调用），
持续FlowForge Event +1.549%（+8.879ns/provider）；record−1.350%（−3.349ns/输出），typed Pose−0.667%（−9.325ns/provider）。
这些单位差值摊销完整批量，不是单独的保护或resume成本。全部配对、绝对时间与每run尾部见详细报告/原始JSON，
不锁频的进程波动仍在；8配置小批量重建尤其波动较大，没有删样或启动新优化。

恢复/组合验证：17 READY按预算3在真实step1–6完成，Channel reset后结果仍为73，shutdown前队列0；
持续sequence停产后合法10步完成450000次恢复，未用shutdown取消冒充drain。多worker/Hook完整窗口诊断
worker=21、owner=82、违规重叠0。8/8192配置各只重建1实例128次，槽位访问128、endpoint计数访问0，backing不增长。
记录固定存储、typed规划上界、VM/EXE-local诊断与进程峰值，不把零计数当全域零分配，不把backing当RSS。

历史参照本轮未重测：原C0→E scalar+9.151%、coroutine+8.099%，H→E scalar+25.648%，
特定protected手写输出→E约116.4941ns残余均保留原身份和限制，不重标为历史→F，不认定全部为必要安全成本。
新增变化不能用旧债务豁免；当前没有发现新的联合正确性缺陷。本次接受带明确残余结束实施，不授予性能等价或全部成本合理。

两项无效开发尝试（遗漏稳定Hook的consumer、日志参数错误的memory脚本）及其修正通过记录均归档，未混入有效性能样本。
main七项未知文件、依赖工作树和入口427个EXE/DLL哈希均未变化；没有合并、tag或冻结。

- [详细职责、C01—C13、类型/方向、作者示例与逐对结果](script-system-sr6-qualification-2026-09-07.zh-CN.md)
- [原始资格/成本、哈希和固定下载入口](evidence/script/sr6/README.md)

`177b110e`原交付状态：SR-6联合候选及明确残余，等待最终审阅；不自动扩功能或进入下一阶段。
当前状态：限定范围验收登记完成，等待用户另行确定后续目标；不创建SR-7、不开始R1、不合并main、不发布tag或冻结框架。
