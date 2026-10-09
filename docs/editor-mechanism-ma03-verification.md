# Mechanism / Authority MA03：RuntimeObject 的空值与失败

实现 `710aa1eba317337d0b2e982093e593849439b11b`，分支 `codex/editor-framework-v2`。
lux-cxx 保持 `0a0e7419fc7229df6e372cd35a540249f92250ef`。本阶段 Windows 范围通过；整体未完成。

## 修前证据与实施

真实旧 SDK 的 string_view 测试确认 RefType 的语义身份、hash、布局及 traits 与 builtin 路径一致，
但未登记时特殊构造静默返回 empty（exit 42）。删除该特殊构造，使用现有 typed construction；
新 SDK 同一夹具在登记/未登记时均成功，元数据指针均为 builtin。字符串字符仍由调用方保活。

`defaultOf(const RefType&)` 明确区分 INVALID_TYPE 与 DEFAULT_UNAVAILABLE；普通堆 OOM 仍 fatal。
`clone() const` 返回独立 owner，区分 COPY_UNAVAILABLE / COPY_FAILURE，失败保留源。
empty 是合法状态，其 clone 成功返回 empty。删除 copyTo、cloneImpl 与原就地复制分支。
字符串工厂不再静态缓存 Registry 元数据指针；登记前失败、登记成功及 Registry 重建均有回归。

核对还发现原反射辅助函数对 RefClass 本身生成复制操作，旧 SDK 的实际 std::string copyTo 失败（exit 42）。
辅助函数、两个内置登记点及 inja 模板统一使用被反射的 C++ 类型；未新增复制算法或元数据目录。
动态 DLL 的代码仍由原 ReflectionRegistrationDraft/Registry pin：外部 owner 释放后复制、两个析构尾部通过，
值先销毁，Registry 随后释放最终代码 owner。

三处 Flow 调用完整迁移。自动默认常量仍是可选状态；显式 reset 返回准确失败且不丢原常量。
原 Literal codec 的失败分类保留。没有提前重做 MA08 的图结构。

## 固定实现验证

独立 clean tracked 检出 `D:/LuxQualification/ma-source`，ValidateTrackedSnapshot 通过。
Editor/PLAYER 在原构建目录增量执行全量 `all -j 4 -- -k 0`，第二轮均 no work；不称新的冷构建。

| 配置 | 实际结果 |
| --- | --- |
| Editor | 124/124，含原 GPU/desktop 回归 |
| PLAYER | 61/61 |
| 全新 SDK 原消费者 | 15/15 |
| RuntimeObject / 实际 DLL / Flow 安装消费者 | 3/3；公共头 C++20 独立编译通过 |
| 已安装生成器与真实模板消费者 | 1/1；删除输出后重建、再次测试通过，第二轮 no work |
| 同一 string_view 夹具 | 登记/未登记均通过 |

源码和 PLAYER 的逐名称增量均只有 meta.runtime_object、meta.runtime_object_plugin、flow.constant_default；
无原测试删除。SDK 原 15 项名称和断言保留。实际 include/link/install 闭包没有 legacy、源码私有头或构建 DLL 补全。
三个模块公开头同步 Debug、RelWithDebInfo、Android；Android 仅同步。已安装 runtime 模板与实现字节相同。

首次测试夹具的 string 花括号窄化编译错误保留。生成验证最初误以为产品存在 runtime-reflection 输出，
实际产品只产生静态投影；补用真实 SDK 生成消费者。其初次缺少 toolset、CMake 模块扫描响应文件及
自身头 include 路径的失败记录均保留；最终消费者直接使用已安装生成器、模板和公开头，不手工伪造输出。

## 证据及保留范围

新 SDK：`D:/LuxQualification/ma03-install`。
外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma03/evidence/`，
1365 文件、46 次实际命令，含前后夹具、原始日志、生成输入/输出、File API、编译链接输入、实现差异与核验脚本。
manifest SHA256：`9562c76532545c4a1668096d57c71df04b0f51f2d1c54f504563caf43e03e05b`。

六处用户修改逐字节不变；ProjectBuilder 补丁未应用；main、历史记录未修改。
LR08 PARTIAL、Linux 未通过、原生输入延期、IME 未测、Q-LR03-HOST-MINIMIZE OPEN 和历史 skinned WAR 保留。
本次未重跑 sanitizer 或旧性能长测，不扩大旧资格。后续按 MA04–MA11 继续。
