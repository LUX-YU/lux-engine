# P07 R1 验收与交接

本轮仅修正 R07-B01：MaterialCompileOperation 与 FlowCompileOperation 的隐式复制／赋值会产生重复的公共控制 owner。停在 P07 等待复审，不进入 P08。

前置验收 `f6598e65f9d77620f1374065a76d82e6e6e00d79`；实现 `85ff8e23902db99b41c5df0b61d2b89985c198f1`。验收绑定该实现的独立干净检出。唯一可变施工材料是 `.internal/editor-redesign/`；此目录及账本为本轮冻结记录，既有 P07 和更早快照保持不变。

## 修复前的真实证据

`before/` 保留实际 SDK 消费者源码、CMake、安装头、编译数据库和原始日志。附件 `spec/` 中的声明级探针只作复审输入，不作为引擎运行证据。

- 实际公共头 traits：两类的 copy constructor、copy assignment、move constructor、move assignment 均为 true。相同 SDK 编译八个目标 static_assert，全部因断言失败而拒绝。
- 真实 MaterialSession + ExecutionRuntime：worker 的 TaskInfo.finished 已成立且未派发业务完成时，复制 `*unique_ptr` 并销毁副本。原 unique_ptr 仍在，32 轮 collect/dispatch 后 `delivered=0 ready=0`。
- 真实 FlowSession + FlowCompilationService(capacity=1)：同一时机把 const 借用复制成值并销毁；原服务仍在，`delivered=0 ready=0 acknowledge_busy=1 capacity_stuck=1`。
- 两种不复制的对照均 `delivered=1 ready=1`，作者源编码、current 与 dirty 保持不变。未修改私有状态，未重跑 encoder，也没有 shim 运行替代。

修复前非法复制源码不会作为常规正向测试继续运行。SDK 重装后重新编译该源码，明确因 deleted special member 拒绝；原失败程序仅冻结于 before。

## 唯一 owner 与代码变化

| 对象 | 控制与寿命 |
|---|---|
| MaterialCompileOperation | 工厂返回的 unique_ptr 是唯一公共控制 owner；移动 unique_ptr，不移动或复制其指向的对象 |
| FlowCompileOperation | FlowCompilationService 的 unique_ptr 记录拥有控制责任；外部保留 ID 或 const 引用；借用止于 acknowledge 或服务析构 |
| 内部完成状态 | 继续使用 shared_ptr，让 Task 和唯一公共 owner 保有既有异步完成寿命 |
| 编译产物 | 继续允许复制拥有型结果，包括 shared_ptr 和拥有字段的数据副本；不因此复制任务控制权 |

两个类均显式删除四个特殊成员。生产 CPP 仅把误导性的“last public owner”注释改成“unique public owner”，算法和析构清理逐字保留。没有修改 ExecutionRuntime、三种作者模型、History、SessionState、保存链、Runtime、Run 或投影／GPU 算法。

新增回归放在原 Material preview 测试目录和原 projection-compilation 安装消费者组；不新增业务库、包或公共控制类型。实际调用方原本已使用 unique_ptr／服务借用，无需同义兼容入口。完整文件清单和 Git 内容哈希见 FILES.md、files.json。

## 验收映射

| 项目 | 证据 |
|---|---|
| R07-R1-01 | before/traits.log、before/contract-negative.log；logs/after-sdk-contract.log、after-sdk-traits.log；八份 reject_*.log |
| R07-R1-02 | before/material-copy.log、material-control.log；logs/ownership-material.log；logs/after-sdk-illegal-runtime.log |
| R07-R1-03 | before/flow-copy.log、flow-control.log；logs/ownership-flow.log；logs/after-sdk-illegal-runtime.log |
| R07-R1-04 | 独立 operation 清理、unique_ptr 构造／赋值转移、只读借用、一次业务完成、固定对象链接重试、容量恢复、owner 释放后结果读用；两份 ownership 日志和原 compilation-detail.log |
| R07-R1-05 | 完整 CTest、PLAYER、11 组 SDK、实际文件 IO／后端／依赖负例、所有旧阶段门禁；receipt.json 收录全部命令和结果 |

完整 CTest **133/133**；PLAYER **11/11**；原 11 组 SDK 合计 **54/54**（原 44 项 + 新增两项真实所有权场景及八项编译负例）。所有最终命令均绑定同一实现 SHA。

原 131 个测试名均保留；检查器按原实现 SHA 比较所有既有 C++ 测试体，禁止仅凭总数宣布保留语义。原 X07-01～07 以及 P01～P06/R1/R2、保存完成接收等回归重新执行。原 11 组 SDK 的基线 44 项保留，所有新契约通过安装头和安装库验证，不借源码头补齐。

显式配置 `LUX_EDITOR_MIGRATION_STAGE=P07`；Editor/PLAYER 全量 `all -j 4 -- -k 0`，第二轮均无工作；构建、测试、安装与 GPU 消费者顺序执行。

门禁 `check_receipt.py` 只访问归档证据和固定 Git blob。包括日志哈希、八种拒绝原因、源码算法未变、原断言保留、真实依赖负例、SDK 和 PLAYER。缺失或损坏结果必须失败；生产机器路径不可用时仍可核验。原机器路径作为命令原始记录保留，不作为取证路径。

## 暂留、失败与范围

- 没有新增桥或延期项。原 MaterialCompilationAccess、FlowCompilationAccess 及旧 UI／IO 转换壳保留原限定消费者，最迟 P12 删除；没有第二套编译算法或 owner。
- C01 仍 FAIL，责任 P09/P12；C03 仍 FAIL，责任 P11；C04 仍 FAIL，责任 P12。原失败场景本轮再次取得失败输出；新所有权问题独立修复，不挂到旧 C03。
- 原冷构建失败和 P06 修复证据不变。本轮复用干净检出的已有构建树，不宣称新增冷构建资格。本轮新增测试初次编译问题保留于 development/，不冒充产品缺陷或最终通过。
- P07 后端绑定、现有 GPU 消费者仍在验收内；完整新产品双视口像素／GPU 退休资格继续属于 P10/P13，没有用 CPU key 或 mock 宣布通过。
- 没有修改 modules 公共头，不新增无关的三前缀同步；没有 Android 构建。
- ProjectBuilder.cpp 保持 SHA256 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，未包含在实现或验收提交；main 未改。

实现和验收独立提交，正常推送实施分支后停止在 P07。
