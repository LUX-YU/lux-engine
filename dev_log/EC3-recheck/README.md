# EC3 再核对与补齐

实现 `aa4509995ea511c4a4306fc8477e44184ca39cf5`；基于原验收 `da64134a485b61592175e61d9a86f22058e04a3b`。停止在 EC3 等待复审。
用户应查看工作区 `E:/SyncForder/CodeRepos/lux-engine-ec2`，分支 `codex/editor-redesign-v4`。

本次发现并修复设置首次选择不尊重允许 scope、失败后页面目录不可恢复，以及只读 Save 拒绝前
已经 Apply 的问题。另使 12 个登记窗口复用实际模块的同一类型声明；设置状态分发不再每轮
构造固定拥有型 ID。具体 C0–C9 核对、所有权和范围见 [audit.md](audit.md)。

原 SDK 的两个真实失败输出及对应测试原文在 before/ 和 logs/ 中；首次夹具编译错误、STRICT
因新增测试未分类的拒绝也保留。原断言没有删除，未修改历史快照。

## 本轮实际验证

- 设置修复提交 `5ec163ff8`：全量 all、第二轮无工作，完整 **244/244** CTest（排除用户延期的 native_input）。
- 最终提交：全量 all、第二轮无工作，**28/28** 受影响回归，含设置、真实窗口、Application、
  三工具、来源保持、Host 和新双视口 GPU；没有宣称在最终提交再次重跑全部 244 项。
- 全新 SDK 前缀只预置已核验外部依赖，再安装本轮 Engine；设置三场景、Application、外部骨骼插件
  HEADLESS/WINDOW、原 P11/Runtime/新桌面双视口/两个旧显式 GPU 消费者通过。
- 两个修改的公共头独立 clang-cl C++20 检查；安装文件、头字节与消费者编译闭包核查通过。
- 原归档按原 Git 对象核验通过。本轮基于已有独立干净克隆增量资格，不称重新冷构建。

未修改的 PLAYER、生成器、脚本等路径按原明确 SHA 继承；原生输入、Linux、IME、sanitizer、
真实多屏硬件及旧性能样本范围没有扩大。P12 免验与 EC2 PARTIAL 不变。

原工作区 `E:/SyncForder/CodeRepos/lux-engine`、main 与 ProjectBuilder 用户修改保持原样。
补丁未应用到实施仓；新路径仍为 `editor/authoring/project/src/ProjectBuilder.cpp`。

`python verify.py --repo <含 implementation Git 对象的仓库>` 使用归档相对路径与固定对象取证；
生产机路径只是记录，不作为读取证据的前提。实现与本记录分别提交，不自动合并或发布。
