# MA09：元数据投影兼容性原型

最终实现：`a0bae4458203721ce6996904e9c25818a30f4e86`，原型为父提交 `a669851bf`，
最后提交只补齐安装消费者的生成器接线。lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
本闭包通过；不表示 MA06/MA08 剩余项或整个机制整改已完成。

## 比较结果和保留合同

一个真实 lux-cxx parser job 同时使用未修改的 Engine `type_static_info.template` 与
lux-cxx `serializable.template.inja`。没有手写字段描述替身，也没有第二套解析器。
生成并执行的代表类型覆盖字段数量、声明顺序、成员指针和实际成员访问、private 排除、
独立 skip 规则、名称覆盖、嵌套值、非连续枚举、vector 和 array。

最关键的结果：两个投影都可能得到七个有效字段，但字段集合不同。
Engine `skip_static` 排除字段描述；serialization `skip` 保留描述并标记跳过。
serialized name 不改 Engine 成员名，enum_meta 的名称表也不是 TTypeStaticInfo 的职责。
因此保留同一解析器、多份领域投影，不把 Inspector 可见性绑定到序列化参与规则。

Runtime reflection 继续服务现有 cold/dynamic 和 FlowForge 消费者；新热路径应优先采用
生成的类型化操作，额外 runtime-reflection 依赖须说明原因。原 BinaryReader/Writer、实体引用
重映射、World archive、限制和语义编码均保留。既有 MetaModuleRegistrar 暂留原消费者，
新扩展使用显式登记。本次没有生产算法、公共头、格式或安装 API 替换。

## 资格

- 最终 clean tracked 独立源码通过检查；Editor/PLAYER 均全量 `all -j 4 -- -k 0`，第二轮无新增工作。
  使用既有增量构建树，不称为冷构建。
- Editor **171/171**，PLAYER **97/97**。相对上一矩阵仅各增加 `meta.projection_compatibility`，
  原测试名称和断言未删除。
- 新 SDK `D:/LuxQualification/ma09-projection-r1-install` 的独立消费者 **1/1**，
  模板与生产头均取安装包，源码仅提供正常测试输入；不借用源码生产私有头或旧构建 DLL。
- 明确删除消费者的两个生成输出，重新构建、执行 **1/1**，输出哈希一致，下一轮无新增工作。
- 生成内容不包含 ReflectionRegistry/MetaModuleRegistrar，原型不需要初始化运行期登记表。
- 六处用户差异哈希未变，未进入实现提交；ProjectBuilder 历史补丁保持未应用。

首次开发构建缺少测试输入 include 目录、首次 SDK 配置遗漏 CMake toolset、随后消费者生成器
拒绝模块扫描响应文件的原输出均保留。补正仅为测试 target 声明依赖，并为不使用 C++ modules 的
消费者设置 `CXX_SCAN_FOR_MODULES OFF`；没有放宽生成器拒绝规则或修改生产模板。

## 证据与未完成范围

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma09/projections/evidence`。
72 个文件、34 条命令，含生成配置/结果和构建输入；manifest SHA256：
`d43fb0159f32f062932b05a50aa5f79dada259bb688ddf31df9ad4209f2b0048`。
中文/空格路径搬迁、真实生成证据缺失及篡改拒绝均通过。

此前 `render.transfer_idle` 缺少完成标记的失败仍未解释；本次完整运行通过不改变历史判定。
针对原失败二进制的十次有界调试运行均未复现，仅作为诊断保存，不作修复或重新验收证据。
MA06/MA08 的来源适配与实际图呈现资格、MA10 全局状态审计、MA11 最终资格仍待推进。
Linux 未提供环境，LR08 保持 PARTIAL；原生输入、IME、sanitizer 与历史性能等范围按原记录保留。
