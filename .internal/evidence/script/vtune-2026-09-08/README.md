# 当前脚本瓶颈的VTune原始记录

这是用户在SR-6验收之后另行要求的实际性能归因，不是SR-7、运行时修改或重新资格；原SR-6证据包未改写。

- 分析入口：`d2d24f056010141b235ac8d684ed088e05b5f78e`。
- 实际EXE/DLL来源：`7b5e1dd4f824e0a2746b1dbb0ec2d0dcc57fe490`，与f64cadde生产/模板一致。
- VTune2026.3.0/build632627，i7-13700KF，Windows RelWithDebInfo；硬件事件权限失败，使用软件CPU/调用栈采样。
- [原始包](VTune-script-bottlenecks-raw.zip)：26,731,237 bytes，1,597项。
- SHA-256：`03beae1439c53c0f4191981a7dced6709c557ee1b018dee9ea7a4340244fc1b4`。
- [清单](manifest.json)、[逐项哈希](raw-files.json)、[SHA256SUMS](SHA256SUMS)。本次新包已本地逐项解包核验。

包含19次有效软件采样、9次未采样执行、原生VTune结果和所有无效尝试；不含编译的EXE/DLL/PDB/OBJ/LIB。
本机完整工作目录是`E:/SyncForder/CodeRepos/build/RelWithDebInfo/sr6-vtune-20260908`。
临时AOT映像的只读保留副本在本机generated-images，包内保留其哈希与反汇编，未把无符号地址伪造为源码名。

| 包内路径 | 内容及解释 |
|---|---|
| identity.json、environment.json、plan.json | 固定EXE/DLL/PDB身份、当前代码关系、VTune版本、机器/权限、工作量与采样范围 |
| runs.json、各场景CSV/LOG | 精确命令、所有有效/无效记录；不同run的CPU时间不作为性能改善对比 |
| *-sw-*/config、data.0、sqlite-db、log | VTune原生结果、原始样本及处理数据库，可以通过VTune读取 |
| resolved/* | 使用匹配LuaJIT PDB重新full finalize的summary、self热点、模块、callstacks、top-down和物理函数视图；初始导出仍在根目录 |
| resolved/symbol-identity.json | 当前Lua DLL与vcpkg安装DLL SHA一致；对应PDB哈希 |
| *-captured-python-*.application.log及exit.json | record/typed实际业务数、errors/backlog及真实child退出码；Python父进程使用notrace:trace |
| analysis/findings.json、metrics.json | 28次有效进程业务完整性、实际工作量、CPU和模块份额；inclusive按各谓词的无重叠树枝求并集，不把父子相加 |
| assembly/* | 11项非空源码行/汇编与实际采样CPU；无硬件计数器时不作单指令周期归因 |
| generated-images/manifest.json | 本轮实际加载的临时FlowForge模块路径、大小、哈希；额外Update诊断不替换原两个主样本 |
| invalid-trials.json | 硬件权限失败、错误场景探测、四次stdout未捕获、cmd包装失败及符号覆盖限制 |
| profile.py、launch.py、resolve.py、analyze.py、metrics.py、assembly.py、report.py | 完整驱动和归因计算；additional-diagnostic-plan.json记录额外AOT保留诊断的原因 |

重放读取示例（在本机已执行同类命令）：

```powershell
& D:/Softwares/Intel/oneAPI/vtune/2026.3/bin64/vtune.exe -report hotspots -r E:/SyncForder/CodeRepos/build/RelWithDebInfo/sr6-vtune-20260908/flow-event-10k-sw-1 -format csv
```

解包到新位置时将`-r`改为对应结果目录，源码/符号路径按identity.json匹配；跨机器不能拿不同DLL/PDB覆盖未知符号。
`-show-as samples`导出在本软件采样报告中仍显示CPU秒，未据此捏造独立事件计数。
硬件IPC/cache/TLB/分支等没有取得数据，未命名JIT/AOT份额和采样扰动按报告保留。

[瓶颈、源码证据、优先级和限制](../../../script-system-vtune-bottlenecks-2026-09-08.zh-CN.md)。
