# Legacy 渲染参考源码

`render_legacy/` 是冻结参考实现，不参与当前配置、构建或安装，也不能成为新模块的链接依赖。
需要重现旧实现时，应检出对应 Git 提交；不通过兼容转发头恢复旧接口。

冻结来源：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
[LEGACY_SOURCE_MANIFEST.json](LEGACY_SOURCE_MANIFEST.json) 记录 719 个文件的原路径、冻结路径、
Git blob、模式和大小，供持续保护检查使用。保持机器清单原字节；以 Git blob 比较源码，
避免把 Windows checkout 的换行转换误判为修改。

| 原路径 | 文件数 |
|---|---:|
| modules/function/render | 663 |
| engine/context/rendering | 5 |
| engine/scene/builtin_systems/render | 29 |
| modules/function/ui/rendering | 10 |
| engine/project/plugins/rendering | 4 |
| examples/render-plugin | 8 |

冻结只表达来源和保护边界，不表示旧实现的功能、平台或性能已取得资格。
