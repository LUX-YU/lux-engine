# SR-4 移动替换补正：本轮证据增量

[补正报告与限制](../../../script-system-sr4-move-correction-2026-09-07.zh-CN.md)；
[SR-5 值转换设计（未编码）](../../../script-system-sr5-value-conversion-design-2026-09-07.zh-CN.md)。
旧 SR-4 及 SR-2/SR-3 包不变。本目录不是新的历史性能试验。

- 最终验证源码：`cd7160ff47c8f8bf36a44f7fbbdeb813fabb018b`，独立 clean clone、RelWithDebInfo。
- 归档提交：`10340084ca58e60d49d35e0f7e00a8109ef24343`。
- 仓库相对归档：[final/SR4-move-raw-evidence.zip](final/SR4-move-raw-evidence.zip)。
- [固定提交 ZIP 下载](https://github.com/LUX-YU/lux-engine/raw/10340084ca58e60d49d35e0f7e00a8109ef24343/.internal/evidence/script/sr4-move/final/SR4-move-raw-evidence.zip)。私有仓库使用既有认证。
- [固定提交逐文件索引](https://github.com/LUX-YU/lux-engine/blob/10340084ca58e60d49d35e0f7e00a8109ef24343/.internal/evidence/script/sr4-move/final/raw-files.json)。
- ZIP 2,070,195 bytes，1,150 个条目，全部逐项核对尺寸和 SHA-256；无 EXE/DLL/OBJ/PDB/LIB。
- ZIP SHA-256：`2b92bee0aedc72fe116818c1a81e4631e3c6cfc0efc6abf965eb075f6a89d6d1`。
- [SHA256SUMS](final/SHA256SUMS)、[源码/依赖/安装身份](final/identity.json)、[实际安装链接闭包](final/installed-link-closure.json)、[测试 EXE 与 build/install DLL 一致性](final-executables.json)、[远端重取校验](remote-verification.json)。

| ZIP 内部路径 | 阅读用途 |
|---|---|
| evidence/final-results.json | 只选最终 cd7160ff：Toolchain 108/108、Developer 120/120、14 个消费者；两个 Lua 消费者明确使用本轮 Developer 优先重试 |
| evidence/before/ | 新编译测试链接固定 SR-4 DLL；活动旧目标清理计数全部 0，首个 EndPlay 断言失败；命令、日志、EXE/DLL 哈希 |
| evidence/qualification/{t,d}/Testing/Temporary/LastTest.log | 新 MOVE 同步/异步/自移动/最终清理标记、busy 子进程、旧七点重入、Event/pin/frontier/预算/Timer 与 Lua 逐 step 原始输出 |
| evidence/qualification/qualification.json、{t,d}/*.log | source_sha 分开保留；最终 all、CTest、第二轮 no-op、install、tracked 检查 |
| evidence/consumers/、evidence/consumers-retry/ | 14 个真实 installed consumers 的配置/生成/构建/运行与链接行；原前缀顺序错误保留，重试只有 Lua packager/authoring |
| evidence/iteration-return-error/ | 首次修后新测试错误期待 INVALID_ID；实际关闭入口契约为 STOPPING，测试单独补正，未改生产返回行为 |
| evidence/invalid-long-path/、evidence/invalid-trials.json | 长路径 MSVC C1083、下游 fixture 阻塞、最初测试编译错误和两项安装前缀错误的原记录与分类 |
| evidence/start-identity.json、preservation-check.json | main、五个 tracked 修改、两个 untracked 文件和固定依赖未变 |
| evidence/move-correction.patch、qualify.ps1、retry-lua-consumer.ps1、prepare-evidence.py | 本轮补丁和验证/归档选取的实际命令，未复制旧证据包 |
| identity/ | 同归档旁的安装身份与实际 link closure；四个 SR-4 owner 私有头未泄漏 |

最终证据来自新执行；不使用上一轮 107/119 结果替代本轮结果。修前只完成第一个安全计数失败场景，不声称全部新用例在旧版跑完。
整个 runtime 关闭后的旧 completion 精确返回 STOPPING；它与 transport 尚开放时的旧实例 INVALID_ID 不同。
busy 是精确退出 73 且资源 retained=1 才通过，普通崩溃不计通过。

远端校验使用本任务 build 目录内新建的临时 bare repository，直接 fetch 上述 GitHub 固定提交，
git show 读取 ZIP/索引/SHA/测试产物清单并校验；不从本地源仓库取同名 ZIP 冒充下载。
该过程复用既有 fetch/show/manifest 方式，没有另建上传系统或重新传送旧包。
