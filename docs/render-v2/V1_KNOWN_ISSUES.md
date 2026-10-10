# V1 frozen reference：已知限制

冻结 SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
状态：`REFERENCE_WITH_KNOWN_LIMITATIONS`。R0 不修 V1，不改历史判定；下表不宣称根因均已确定。
路径均为历史 Git 原路径，可用 `git show <receipt-commit>:<path>` 恢复；本轮未删除这些文件。

| 事项 | 保留事实 | 来源 Git path / 收据 commit | 原实现 |
|---|---|---|---|
| render.transfer_idle | lost 分支缺最终 PASS 标记，输出停于 transfer idle boundary；原因未确认 | `docs/editor-mechanism-ui-actions-verification.md` @ `0f5342e8443e9b7ee77cba6715fdf5f27bf93917` | `80f82db40602815a80bf060867889a3bdf1bc44b` |
| skinning cross-frame WAR | 历史 GPU 同步问题仍保留，没有被后续普通回归关闭 | `docs/editor-mechanism-ma11-verification.md` @ `40286bf1fdd687901c1dd45aa395cc96c3c48046` | `7d84e8785ce6778e180d9f78cbb6e96bdd409fb5` 的收据重申既有问题 |
| minimize | `Q-LR03-HOST-MINIMIZE` 仍未关闭 | 同上 | 同上，非本轮复现 |
| Clang/UBSan | Clang 19.1.5 trap 完整 CTest 100/130，30 项失败，PARTIAL | `docs/editor-mechanism-ma11-clang-trap-verification.md` @ `7781f1b03853527984550837a9ddd22c1a4c0155` | `e0faf423e1e7fbc3e160e1341d891891d24970fc` |
| UBSan 后续分类 | 回调编译探针单项修正不等于新的 101/130 全量运行；不同 trap 不合并为单一根因 | `docs/editor-mechanism-ma11-sanitizer-classification.md` @ `a669409a289a6fa4092f21176397795b1cdb7f3e` | `61007eedca8c3feb627ae00feb8b0f13ee0b4b56` |
| Linux | NOT_RUN / 未满足；没有 Linux 环境，不用 Windows 替代 | MA11 verification @ `40286bf1fdd687901c1dd45aa395cc96c3c48046`（完整路径同上） | `7d84e8785ce6778e180d9f78cbb6e96bdd409fb5` |
| native input / IME | 原生输入 NOT_RUN_USER_DEFERRED；系统 IME 未测 | MA11 verification @ `40286bf1fdd687901c1dd45aa395cc96c3c48046`（完整路径同上） | 同上 |

MA11 收据还保留实际图编辑器呈现缺口、Material VERTEX_COLOR 原后端失败与旧性能延期；
该简表不替代原收据、不抹去其他限制。

已核验的外部归档锚点（均位于 `EditorFramework-v2/` 下）：

- `mechanism-ma07/actions/evidence`：94 文件；manifest
  `06b7e3c6aafa342ddab4b782ff2ebc8bff2731f4f96b317261a29a999f23605e`。
- `mechanism-ma11/evidence`：5818 文件；manifest
  `aeb2e53a43613362c6b97b44c7f39224a2ace5d3197d4690df33e0c9fdf58ead`。
- `mechanism-ma11/ubsan-trap-evidence`：138 文件；manifest
  `44116f2ab1b9ff0c5d020723d1292a3f0b881f68d9fba316a4499d627dfb4e1b`。
- `mechanism-ma11/ubsan-classification-evidence`：119 文件；manifest
  `a00d0f52e846b45b076a089336b7bd66447146b25b272bb4a9a5c482fe9bd84e`。

核验归档完整性不代表重新运行其中测试。frozen SHA 四配置、GPU、SDK、性能均为本轮 NOT_RUN。
