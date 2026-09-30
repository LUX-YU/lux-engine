# P10 R1 施工记录

当前分支 codex/editor-redesign-v4，HEAD b9b856477755a9247a7a6810fa880ae62151f565，与原 P10 验收一致。ProjectBuilder.cpp 的用户差异和哈希已确认。复审包 manifest 全部匹配，包内试验说明不当作执行证据。

范围仅为 Material/Flow UI 草稿与有界输入队列的来源，不改模型、History、Store、Host、Runtime、Workspace 或执行器。先通过原安装 SDK 的正式 Flow 属性控件/Apply 及 GraphCanvas 接线验证原问题，再补正具体工具，最后显式 P10 完整矩阵、独立实现/验收提交和推送，停在 P10。

受控 ImGui 激活/文本输入和 GraphCanvas 信号用于准确安排先后顺序；不宣称这些是 OS 鼠标或 IME 测试。原 P10 真正原生输入/GPU 测试保留并重跑。

实现提交：653f83018be6b387cc78320b06d9891881f96dc6。生产改动仅两个工具 UI CPP；测试/测试依赖登记共五处。无模型、运行、执行器、Host、保存、Workspace 或 widgets 改动。

四个 before 已冻结到 P10-R1-before，均为原 SDK 实际 exit 1、source_and_history_preserved=0 stale=0。补正后的新增10场景及 native History/checkpoint 断言通过。首次开发构建缺新增测试头白名单、随后开发编译的聚合初始化/ClosePermit 借用错误都保留日志，未放宽生产规则。ProjectBuilder.cpp 哈希不变。

从该实现提交的独立 clean clone 显式配置 P10，全量 all -j4 -k0 和二次 no work 已通过，195/195 CTest 通过。余下安装消费者、PLAYER、重复专项负例和两旧显式 GPU 正在顺序执行；未在结果取得前计为 PASS。

最终断言补强：两个队列 BUSY 时明确验证 overlay 原值 x=20、恢复 PREVIEW 后 x=25；签名 S1 同时改变 arguments/results。没有改生产代码。未推送的实现提交已合并为 514bca1f180e91a88545d26d5d11a63fcd7e7498；653f8301 是中间测试版本，不能替代最终提交验收。原中间输出保留。
最终版本的第一次配置在构建前主动中断：其启动 shell 曾为本地测试加入开发树 bin。保留配置输出和中断记录；后续完整验证从独立 shell 启动并显式过滤开发树 DLL 搜索路径。已有 git/祖先/clean-snapshot 读取结果可复用，未将该配置计为通过。

最终 SHA 的全量 195/195、PLAYER 11、SDK 前13组通过。额外 workspace-catalog 一次 0xC0000409 退出：实际 dump 的 std::filesystem::rename 抛出 filesystem_error，Windows 错误5。保留失败命令、空输出、dump、PDB分析与物理文件；不修改 Workspace 或测试。CDB下重跑及10次同参数普通重跑均通过，具体占用来源未知。正式顺序脚本随后同命令通过，继续剩余桌面/SDK/GPU和归档检查。脚本只复用同 SHA、同命令且日志哈希吻合的成功记录；原失败日志另存。

最终实现 514bca1f 的执行矩阵完成：195/195 native、PLAYER 11/11、14组 SDK 106/106；原185行为及断言、依赖负例和8项operation编译负例保留。新双SceneView GPU/native输入与两旧显式GPU模式均通过，显式P10门禁及原P10归档门禁通过。C01/C03/C04按原FAIL复现，IME保持NOT_RUN。开始冻结dev_log/P10-R1并执行缺失/损坏/生产路径不可取得的归档验证；不进入P11。
