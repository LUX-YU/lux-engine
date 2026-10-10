# Implementation Style & Philosophy — mandatory index

**本文件只是导航，不产生第二份风格真相。** 实施者必须完整阅读并执行以下三个权威章节：

1. `01_哲学_原则_完整性.md` — 语义先于类结构、单一权威、完整 Legacy、共享 Scene/View。
2. `12_Cpp20_实现哲学.md` — plain values、强类型 ID、free algorithms、Concepts/`if constexpr`、短期 Builders、move-only RAII、代码生成/插件 ABI、LLM 代码外观硬禁令与 type inventory。
3. `13_错误_性能_生命周期.md` — 构造即完整、`RenderResult`、错误语义、三 Lane 并发、fence-proven GPU retirement、热路径零分配测量。

每阶段的否决与验收由 `16_验收与功能矩阵.md` 和 `17_LLM实施合同.md` 负责；`19_冻结决策与歧义消解.md` 已关闭设计选择。任何 Style 约束不能用于裁剪功能、简化同步或抛弃 GPU Oracle。
