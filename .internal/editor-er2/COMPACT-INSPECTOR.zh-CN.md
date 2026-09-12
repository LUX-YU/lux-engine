# Inspector 紧凑向量布局

基线 6a89cd1654d2c803a1a09fb561bf75deb107bcd4。用户要求 Translation / Rotation / Scale 的三轴同排。

生成器直接输出 SameLine 与按可用宽度计算的控件宽度；预留各轴标签和间距。固定向量同排、矩阵逐行，
四元数仍使用原有角度转换，三轴同排且悬停提示 Degrees。各轴 ID、校验、Session 和历史协议不变。
没有修改底层模块、资源协议或原有资格产物 E:/lux-er2-gen-q3。

本轮 MSVC RelWithDebInfo 开发构建 E:/lux-shadow-ui/msvc 的 all / -j 4 / -k 0 通过，第二次 no-op。
受影响 CTest 4/4：生成类型、生成契约、场景编辑和局部历史。新增实际 UI 几何检查在 340/1000 像素宽度
分别确认向量及旋转同排、不重叠、不超出窗口；三个向量分量逐个拖动，只修改对应值并可 Undo 恢复。
真实 Scene GPU editing 变体通过，七次作者投影/回放图像、实际 Inspector 输入、Edit 菜单、Ctrl+Z/Y、
取消、失焦、隐藏及完整 owner 关闭通过，validation_errors=0。输入事件测试不等于原生桌面验收。

原始记录 E:/lux-inspector-compact/logs；本轮只做受影响验证，未重跑完整 SDK/诊断/成本矩阵。
以前候选的 139/159 和 SDK 资格保持其原身份，不重新标记为本轮结果。

阶段澄清：v4 §20.2 明确首批为 Transform/Light，未默认授权对象创建删除或其他资产引用修改；
§20.3 为 MaterialGraph/FlowGraph 迁移，§20.4 为联合切换验收。当前 Resources 面板是场景资源请求状态，
没有迁入完整资产浏览器。浏览器与拖放应另立场景编辑续行范围：先资产浏览/选择和 typed 引用替换，
再模型实例创建、放置和撤销；从外部模型文件导入还涉及已有 Toolchain 导入入口。本轮不实施这些扩展。
