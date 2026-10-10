# F1-FIX-2 — F3 准入前必须解决的两项缺口

这是用户明确要求的未完成合同追踪，不是另一套架构规范或已实现功能清单。
本轮只登记，状态均为 **NOT_IMPLEMENTED / OPEN**；F1-FIX-2 的 PASS 不关闭这些项。
在 F3 生产实施开始前，必须取得明确授权完成修复、真实资格证据并经用户审阅。
不得以 TODO、已有有限 fixture、R4 GPU smoke 或将错误交给 Vulkan driver 代替关闭证据。

## F3-PRE-01 — 整数 RenderTarget Clear

当前 `Attachment::clear` / `GraphFieldBinding::clear` 只有 float[4]，不能完整表达
有符号和无符号整数 RenderTarget 的清除值。格式的整数分类通过，不等于 clear 值类型正确。

必须交付：与唯一格式权威一致的 float / signed integer / unsigned integer clear 值合同，
明确格式与 clear 类型的匹配、错误表达、捕获和 native 转换责任，不允许静默数值转换或重新解释 float。
Depth/Stencil clear 保持各自独立的语义。类型形状及实施范围在修复工作单中冻结，本轮不预造 API。

关闭证据：实际 signed/unsigned target、负数与大于 float 精确整数范围的 unsigned 值，
合法捕获与错误类型编译或构建拒绝；Schema/Definition/native clear 逐项对应。
在 F3/F4 对应 native 能力具备时补齐真实 clear → readback 数值 oracle 和 validation 0 error；
前置作者修复通过与后续 native 证据须分别记录，不以一种证据冒充另一种。

## F3-PRE-02 — Shader Include 与生成声明顺序

当前 emitter 将开头的 include/extension/define 放在生成声明前。
既有 LegacyTonemap 与 StageCommon fixture 只证明各自输入成立，不能证明通用 include 组织正确。
include 中的函数若读取生成的资源/参数，会在声明之前使用名字；相反，生成声明也可能依赖 include 提供的类型。
不能简单移动所有 include 或复制最终 set/binding 宣称解决两类依赖。

必须交付：在既有 .lglsl 生产 toolchain 内明确定义 version/extension、类型声明、生成字段、
依赖这些字段的 helper/include 与 body 的合法顺序、入口及错误诊断。使用既有预处理/生成链，
不新增第二个 C++/Shader parser、reflection authority 或手工绑定旁路。

关闭证据：真实 Legacy 风格头部；include helper 访问生成字段；生成声明使用前置类型；
传递 include 与宏/条件组织；非法晚到 version/extension 或无法满足的顺序应明确拒绝。
所有正例经生产 emitter → glslc → SPIR-V reflection → Schema validator，负例验证准确错误，
不以手写最终 GLSL 或只检查字符串顺序代替真实编译。

## 放行规则

F2 不得隐式承担这两项修复，也不得将其登记为完成；需单独批准相应文件范围。
进入 F3 前必须核对两项前置作者合同的实现/验收 SHA 和原始证据。
后续 native 验收义务继续由正式 F3/F4 工作单承接，尚未具备执行机制时保持 NOT_RUN。
