# Script v4 原始证据

唯一归档：[script-v4-raw.zip](script-v4-raw.zip)，[SHA-256](SHA256.txt)，[身份](archive.json)。
解包后 `MANIFEST.json` 提供各文件路径、字节数及 SHA-256，打包后已逐项重读验证。
最终 Engine 资格固定 `f7d2815bdd2025ee23a7c11449def822413f58e9`；原 v3 对照为
`e06208deb29b9b1de52fe49a129091f693bb7e08`。归档/报告提交不产生新测试身份。

| 入口 | 证据 |
|---|---|
| `start.json`、`fixed-dependencies.json`、`final-audit.json` | 初始未知修改、安装依赖、最终头同步/身份复核 |
| `w0-api-before-run.*`、`w0-api-after-run.*`、`final-api-*` | 内层栈修前断言、修后深度/OOM及最终 API-check |
| `w1-profile/` | 本轮一次软件 Hotspots 原始数据库与导出；完整进程采样，不是 PMU/精确 ROI |
| `w1-diagnostic/`、`resources-analysis.json` | 页 class、时序及配套算法的观察身份 |
| `w3-experiment/`、`w3-budget32/` | GC三选项、独立32MiB；包含未采用候选 |
| `w4-resources-*`、`w5-*` | 子提交构建、真实容量/重入/失败清理/生成用例 |
| `final-{t,d}-*`、`final-vm-tests.*` | clean clone 全量构建/无工作/110+126 CTest/4 VM合同 |
| `installed/`、`value-incremental/`、`relocated/` | 15 SDK消费者、13增量、两条迁址纵向链 |
| `final-costs/`、`short-backends/` | 原五腿30进程、两后端42进程；AB/BA/AB、所有业务行及总时间 |
| `memory-diagnostic/`、`other-resources/` | 单独的内存、allocator和frame观察；不作为正式计时 |
| `hot-storage-machinery.*`、`final-*-disassembly.log`、`frame-layout.*` | 当前机器码、384B C++本体及安装头布局 |
| `*-image/identity.json` | 各阶段 EXE/DLL/VM/生成资产哈希；产物保留在本地同名镜像 |

归档保留失败构建和驱动错误日志；是否有效以每条命令的退出码、实际 oracle 和最终报告为准。
不包含编译二进制；VTune 数据文件是采样记录。原始产物保留于
`E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-v4/`，原 v3 镜像未覆盖。
结论与固定提交下载入口见 [RESULT](../../../script-v4/RESULT.zh-CN.md)。
