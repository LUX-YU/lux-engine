# EC3 续验完成，等待复审

隔离解除后完成剩余 Windows SDK、插件、生成器和 GPU 验证。EC3 本轮规定范围已经完成；
EC2 的源码及 SDK 原生输入仍按用户安排延期，P12 免验、Linux、系统 IME、sanitizer 和旧性能
样本限制不变。本记录不把这些范围改判为通过，也不确认 Defender 检测是误报。

查看工作区：`E:/SyncForder/CodeRepos/lux-engine-ec2`，分支 `codex/editor-redesign-v4`。
实现提交：`0a521da070c1f4addb511ea038f8040f265702f6`；本目录是其后的独立验收提交。
lux-cxx 依赖仍为 `f5447a763fd042f4f08819c8c88f1676f9a2a3cb`。

## 本次实际执行

- 23 组原有 SDK 消费者全部通过，包含实际插件、三类作者模型、保存、运行、交互、工作区、
  新双视口及两个原显式 GPU 模式；逐个名称和日志见 `receipt.json`。
- 独立声明寿命、项目保存真实 IO、工作区 Host/文件、V9 骨骼插件、EC1 和 EC2 脚本消费者通过。
  工作区消费者没有 CTest 登记，保留“无测试”输出，另直接运行 executable 验证，不将空结果算通过。
- 九项声明编译负例命中原静态约束；原八项 operation 特殊成员负例保持拒绝。
- 安装的 MetaUnit/inja 工具在中文及空格路径完成 25 个事件检查：字段与传递头、模板、support、
  flags 增量、无关头、缺失输出恢复、模板/格式化/真实发布失败及恢复。
- 实际生成的暂停 Run 数值控件完成 Preview、Commit、Cancel、Undo/Redo；作者完整编码、历史、
  current、dirty、observed 和绑定均不变。此项运行同时包含真实双视口回读和 EC2 R1 球体/四边形
  配方回归，验证层错误为零。
- 完整安装清单 10,309 条无缺失，新增公开项目命令头与源码字节一致，旧 Python 生产 emitter
  不在安装目录。编译数据库检查没有 source 私有头或旧 Engine SDK 前缀借用。

原 Run 手势草稿误把 SceneReadView 当成具有 `encode()` 的接口，首次真实编译拒绝已保留。
修正测试为公开的 `capture → buildSceneSnapshotPackage → encodeScenePackage`，没有为测试
扩张生产 API。失败草稿、修后 patch、源码哈希及实际执行输出分别归档。
安装核验脚本也纠正了 ProjectCommands 的物理路径假设：公开目录为 storage，而非 project；
没有再次复制头或更改生产接口。

## 准确的源版本与继承

241/241 源码回归、20/20 PLAYER、独立 cold all 及二次无工作在 `17ea8c40` 执行。
后续 `0a521da0` 仅补一行 SDK 安装清单和消费者的头引用/四项断言，没有修改生产算法或头内容。
该提交的 all、二次无工作、五项受影响回归和 56 个公共头 clang-cl C++20 检查已记于 `../EC3-C9/`。
此次 SDK、插件和生成 Run 控件结果绑定 `0a521da0`；不伪称重新冷构建或重跑所有源码测试。

`../EC3/` 和 `../EC3-C9/` 的隔离、失败、PARTIAL 和取证限制保持原样。
本次 DLL 恢复后 SHA256 与原构建一致；没有改变安全设置、排除规则或借用 build DLL 绕过安装验证。

## 成本与范围

实际尺寸仍为 Descriptor 104、Entry 136、Handle 16 字节。35/256/1024 项目录各执行稳定菜单、
冷查找、替换、PINNED/CURRENT 测量。稳定菜单 1000 次在计数范围内零分配；命令派发仍有
1002 次分配、104192 requested bytes，不能称整个路径零分配。调用方 ID 创建与 DLL allocator
未计入该局部统计，时间原样保留，不补旧长测或宣传未经证明的提速比例。

V01–V60 逐项证据见 `coverage.json`；实际 owner、迁出和删除项仍沿唯一施工账本冻结的
`ledger-snapshot.json`。多屏负坐标和拔屏等来自合成环境，不宣称真实多屏硬件覆盖。

## 保护与核验

原工作区 `E:/SyncForder/CodeRepos/lux-engine` 仍位于原提交，仅保留用户的 ProjectBuilder 修改，
未应用到新工作区、未提交。对应新路径为 `editor/authoring/project/src/ProjectBuilder.cpp`。
用户文件 SHA256 为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，main 未动。

`python verify.py --repo <包含所列 Git 对象的仓库>` 使用归档相对路径及固定 Git 对象核验。
命令日志中的生产机绝对路径只作记录，不作为取证依赖。缺失/篡改负例仅操作临时归档副本。
停止在 EC3 等待复审，不自动推进下一阶段、合并 main、删除分支或发布。
