# Mechanism / Authority MA04：日志输出的宿主寿命

实现 `fd3a7665988f6c843ea91b20a1c9d7cd968ee29b`，分支 `codex/editor-framework-v2`。
lux-cxx 保持 `0a0e7419fc7229df6e372cd35a540249f92250ef`。本阶段 Windows 范围通过；整体施工未完成。

真实 MA03 安装 SDK 的修前夹具证明：清空旧 setOutput 后，捕获 owner 仍因 retired 列表存活（exit 42）。
新接口借用宿主拥有的稳定 LogOutputTarget，宿主停止并 join producer 后才替换/清空，再释放状态与代码。
删除 OutputFn、setOutput、retired owning closures 和输出 mutex；原过滤、格式化、序号及 stderr 回退保留。
当前实际产品没有旧 setOutput 调用，继续使用 stderr。没有增加日志线程、队列或共享 owner 热路径。

日志专项实测两轮、每轮四线程，共 8,000 条记录：同步 producer 线程执行、每线程顺序、唯一序号、
过滤、清空后立即释放 owner 均通过；安装前及清空后的实际 stderr 输出保留。
LogRecord 仍含借用格式/参数，延迟消费必须由宿主承担相应内容与代码寿命，没有声称按值复制即可安全排队。

## 固定实现验证

独立 clean tracked 源码 `D:/LuxQualification/ma-source`，ValidateTrackedSnapshot 通过。
Editor/PLAYER 复用构建目录增量执行全量 all -j 4 -- -k 0，第二轮均 no work；不称冷构建。

| 配置 | 实际结果 |
| --- | --- |
| Editor | 125/125，含既有 GPU/desktop 回归 |
| PLAYER | 62/62 |
| 全新 SDK 原消费者 | 15/15 |
| 日志安装消费者 | 1/1；公共头 C++20 独立编译、直接 stderr 输出核验通过 |

源码与 PLAYER 逐名称比较仅新增 log.output_lifetime，无原测试删除；SDK 原名称和断言保留。
实际 compile/link/File API 闭包通过（663/607/57/2 个编译单元），log 不依赖 engine、Editor 或 UI。
两个公开头与新 SDK 及 Debug、RelWithDebInfo、Android include 一致；Android 只同步，不作为构建成绩。

## 证据与保留项

新 SDK：`D:/LuxQualification/ma04-install`。
外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma04/evidence/`，
1314 文件、25 次实际命令，含修前夹具、原始输出、File API、编译链接输入、核验脚本与保护哈希。
manifest SHA256：`772b5fd8692f4fb71090c33b3c269c57d99919acb484aa5a693c1bdabb979266`。

六处用户修改逐字节保留，ProjectBuilder 补丁未应用，main 和历史快照未改动。
LR08 PARTIAL、Linux 未通过、原生输入延期、IME 未测、Q-LR03-HOST-MINIMIZE OPEN 和历史 skinned WAR 保留。
本阶段未重跑 sanitizer，不扩展旧资格；继续 MA05–MA11。
