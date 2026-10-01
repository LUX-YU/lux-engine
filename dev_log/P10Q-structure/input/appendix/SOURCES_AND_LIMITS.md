# 本施工包的来源、裁定和制作边界

## A. 已认可设计（原字节附带）

- D1 `reference/00_LAYERED_DESIGN.md`：五层职责、静态/动态选择、依赖矩阵、范围。
- D2 `reference/01_MIGRATION_AND_ACCEPTANCE.md`：原迁移计划L0–L6与后续阶段。
- D3 本对话用户对Linux/旧长测范围的明确调整：本包继承，不用旧收据重新否定。

本包不是再次设计第六套架构。`01_DECISIONS_AND_INVARIANTS.md`对ProjectBuilder的显式纠正以当前代码为依据，其余目标语义与D1/D2一致。ViewError拆分、私有输入算法路径、任务target名字等为本次施工细化，不声称原代码已有。

## B. 本次补读的固定Git来源

所有源码下列URL都固定在 `f7c27f9375cbf8dd8af37b30a6027a460de26213`，除ref查询本身。

| 编号 | 路径／资源 | 用途 |
|---|---|---|
| S01 | Git ref `refs/heads/codex/editor-redesign-v4` | 本次仍指向上述验收SHA |
| S02 | P10Q用户报告（本对话README(10).md） | 204/178/11等历史验证范围、用户文件保护、实际依赖SHA |
| S03 | editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp | 纯观察与关闭错误混合定义 |
| S04 | editor/project/include/lux/engine/editor/project/ProjectBuilder.hpp | 纯Builder公共结构 |
| S05 | editor/project/src/ProjectBuilder.cpp | 验证配置并返回，不执行异步/IO |
| S06 | editor/project/CMakeLists.txt | editor_project实际source与STATIC边界 |
| S07 | editor/editing/sinclude/lux/engine/editor/editing/InteractionDelivery.hpp | 已有单一交付算法与可约束签名 |
| S08 | editor/tools/scene/model/CMakeLists.txt | 纯模型source/PersistenceAccess/真实测试 |
| S09 | editor/tools/scene/projection/CMakeLists.txt | 生产与跨层集成测试依赖区别 |
| S10 | editor/tools/material/ui/src/MaterialView.cpp（开头范围） | 真实共享交付头consumer、工具依赖 |
| S11 | editor/editing/include/lux/engine/editor/EditorError.hpp | 纯错误声明不等于旧业务目标 |
| S12 | editor/editing递归Git tree | 新旧editing与sinclude文件位置 |

源码链接基址：

```text
https://github.com/LUX-YU/lux-engine/blob/f7c27f9375cbf8dd8af37b30a6027a460de26213/
```

## C. 本次没有完成的工作

- 没有修改或推送lux-engine仓库代码。
- 没有对全仓每个文件完成语义审查；71条路径规则是从已读设计和关键代码构造的计划种子，必须在L0展开并核验。
- 没有重跑引擎构建、CTest、SDK、GPU、Linux或完整历史归档checker。
- 没有将原P10Q PARTIAL改为PASS。
- 附带脚本的测试使用本地合成Git仓库；只证明计划输出、安全拒绝和规则处理，不证明引擎依赖正确。
- 目录/target/类型裁定是施工目标，不是已实现的代码事实。

## D. 文档和辅助工具的验证

制作过程将检查本包链接、JSON、原设计字节一致性、计划脚本的合成fixture以及ZIP完整性。记录见 `evidence/package-selfcheck.json`。

如果另有独立C++概念检查，其范围只能是合成状态类型的约束/阶段逻辑，不是实际lux-cxx/Material/Flow/Process或SDK资格。正式实现必须运行本施工定义的真实consumer。
