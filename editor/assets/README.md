# 编辑器资产工作流

AssetImporter 借用 ProjectStorage 和执行设施，组合 engine/toolchain 转换与项目发布。
源打开、编译和保存保留固定版本、任务身份与关闭协议，窗口隐藏不终止已经接纳的任务。

纯格式转换位于 engine/toolchain；运行资产读取/解码位于 engine/process/asset_loading。
本模块只承担编辑器工作流，不建立第二份项目目录或 GPU 资源缓存。
通用 History、AssetEditing、保存票据与错误位于 editor/editing。

`editor_assets` 为 STATIC，无独立资产 DLL；其安装 include 前缀仍为 lux/engine/editor。
