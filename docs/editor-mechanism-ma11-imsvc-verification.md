# MA11：Clang 系统包含路径补正

lux-cxx 实现：`bf779515a120350c7c5412c366eea73afdf59ccf`。
Engine 生成输入：`7d84e8785ce6778e180d9f78cbb6e96bdd409fb5`。
本项 PASS；MA11 与整体整改仍为 PARTIAL。原 MA11 记录和首次 Clang/UBSan 失败不改写。

## 原因与改动

真实 clang-cl 编译数据库用 `-imsvc` 表达系统包含目录。GeneratorHelper 原先只识别 `-isystem`，
导致生成器丢失真实编译环境，报 MissingMarker、SlotMap、EnTT 等依赖解析错误。

只修改 lux-cxx 的原包含路径提取算法：接受分离和紧接形式的 `-imsvc`，复用原系统路径顺序和去重。
没有复制解析器，没有修改公共头、模板、Engine 运行算法或放宽无 RTTI 规则。

原 compile_environment 测试保留全部断言，新增四种组合：arguments/command 与分离/紧接路径。
实际覆盖带空格路径、重复系统路径、普通包含优先级，以及真正解析该依赖的结果。

修前使用真实安装 SDK `0a0e7419fc7229df6e372cd35a540249f92250ef`，新增回归退出 13，
输出 `clang-cl system include extraction failed: joined=0, command=0`。
首次夹具构建缺 nlohmann_json 依赖的失败也保留；补齐夹具后才取得上述真实行为失败。
日志中的 deliberately invalid dependency 属于既有负例，不能另记为本次失败。

## 实际验证

- 从 bf779515 的独立 clean tracked 检出先验证源码快照；新构建树 all 205 步通过，第二轮无工作。
- 原 56 项 CTest 全部通过，新增断言位于原 compile_environment 测试内。
- 全新前缀安装成功；真实安装消费者构建、无工作及回归通过。
- 使用原失败 Clang/UBSan 构建的 47 份真实生成任务，仅重定向输出位置，全部执行成功。
  244 份生成文件与原 MSVC 构建基线逐字节相同；归档保存两份实际字节和映射。
- 开发依赖前缀采用 bf779515，SDK 身份为 dirty=false；四个生成器/反射二进制与新前缀一致。
  另一个仅使用开发前缀的消费者构建、无工作和回归通过，没有借用新测试前缀补全依赖。

首次 `cmake --install --prefix` 未覆盖 toolset 的绝对头文件/share 安装目标，未据此宣告安装完成。
随后重新配置实际 CMAKE_INSTALL_PREFIX，执行 all、无工作和完整安装；首次命令输出保留。

## 证据与边界

外部冻结归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/imsvc-evidence`。
688 个文件、73 条实际命令；manifest SHA256：
`41f6aceb2174d14d710c1aac19bc5c545c4ccc58816e8625a3f8492cf8c8251b`。
中文/空格路径搬迁通过；删除及篡改真实 CTest 日志均被拒绝，恢复后通过。

原 Editor 171、PLAYER 97、TOOLCHAIN 83、ASan 130 和 26 组 SDK 成绩绑定原 Engine SHA 与依赖 0a0e7419，
本轮没有将它们重新登记为 bf779515 的全量运行。本轮验证限于生成器、原 lux-cxx 测试、安装消费者和实际生成输出。

Clang/UBSan 全量尚未重跑，MSVC STL 的无 RTTI get_deleter 限制和 UBSan MT/MD 运行库冲突仍未关闭。
实际图编辑器呈现缺口、Linux 未提供环境、原生输入延期、IME 及既有失败继续按原记录保留。
六处用户差异逐字节未变，ProjectBuilder 补丁仍未应用；main 和历史快照不变。
