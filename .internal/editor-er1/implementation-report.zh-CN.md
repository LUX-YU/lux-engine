# ER-1 续行修复与资格记录

状态：ER-1 尚未通过。主体拆分保留，不进入 ER-2。

本次落实 EX-01/02/03：aligned allocator 传播标准分配异常；callback/payload 准备失败回滚及显式关闭错误保留；不同 target 首触及与共享 import 的 barrier 区分；Control/Program/Upload 共用实际 envelope 预算并轮转。

Toolset 已迁至独立 application/tooling 共享组件。新 Application 在冷装配期提供 typed Toolset，启动冻结，显式关闭按 owner 顺序停止并销毁。Context/Inspector 和安装 DLL 消费者已适配真实头，无转发 alias。

正常 DLL 与专用诊断 DLL 分离。正常构建不含 replacement new/delete 和故障注入对象；dumpbin 导出、CRT/PDB module 来源及 DLL hashes 位于 evidence/continuation/isolation-02。诊断通过实际 DLL 工厂20个、seal6个、client10个分配失败点；seal原index=2保留，每个失败检查输入texture序列及全部image引用数恢复。

本轮中间实际结果：normal-04 CTest 133/133；diagnostic build-61 重载 MSVC 环境后 CTest 134/134。未加载MSVC环境的 ctest-61-all 15个编译负例因标准头缺失失败，日志保留，未算通过。真实GPU覆盖双View/相同尺寸/顺序倒置/resize与关闭、外来Renderer、迟到资源与部分失败、同packet重试、终局录制失败、旧packet和pool重建；具体逐项范围见 acceptance-results.csv 和原始日志。

image_lifetime 使用实际Mesh/Light场景：旧packet保留32步，零尺寸暂停32步，backing 1/3、generation 1/5，descriptor最终6/6，零Vulkan验证错误。最初空RenderScene未产生可采样descriptor而超时的失败日志保留；未借此降低成功标准。

源码候选将提交后用 cmake/RunEditorEr1Qualification.ps1 从独立 clean clone 构建；最终身份记录到独立 qualification.json，不能将上述工作树日志重标为最终结果。

续行的 clean q1 构建发现堆损坏，ASan 已定位并修复新示例重复注册反射造成的 use-after-free，详见 HEAP-INCIDENT.zh-CN.md。q2 因未固定编译器而误选 clang++，全量构建失败，未运行其程序。以上失败均保留。

diagnostic build-65 全量 CTest 135/135，51.21秒；第二轮 no-op。五个独立进程均实际观察 resize 请求2的旧回复晚于请求3，并验证只有最新回复才发布READY。GPU关闭测试用8个真实空StateUpdate回收请求槽的CPU附件，不增加GPU帧；仅剩View owner引用且真实完成水位尚未追上提交时保留资源，最终descriptor 7/7。gpu-62/63 中未正确回收旧请求槽而超时的测试失败日志也保留。

仍未满足的门槛：完整部分启动与终局设备失败矩阵、实际持续RMB/MMB捕获后失焦和中文IME候选、至少五组同量旧新成本配对，以及对应旧正式入口删除和迁移完成。被动RenderLease析构中的既有deferred vector分配还没有获得全路径OOM保证。保留旧正式入口，不声称最终SDK已排除它；不将Text/Record路由fixture写成材质或Scene内容编辑资格。

原build48归档及哈希保持不变。main及并行脚本分支不修改、不合并、不推送、不发布。

后续资格：q4/qualification.json 绑定 bae70cde；它的134/134 CTest、SDK迁移、实际Toolset DLL加载和Scene reader重新生成已通过，完整隔离证据在q4/isolation。相机协议错配是在之后同量比较中发现的额外正确性问题，详见 CAMERA-PROTOCOL-INCIDENT.zh-CN.md；因此不把q4或旧像素哈希重标为新修复后的最终结果。
修复后build-67诊断135/135和19个Scene GPU变体、foreign/lifecycle重跑通过。正常成本入口独立记录实际帧数、像素、工作/等待与CPU，并排除专用故障代码；500帧试跑已得到完全相同的旧/新图像，正式五组尚待tracked候选clean clone。
