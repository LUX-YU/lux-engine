# Editor 产品宿主

## 主菜单与工作区

EditorLoop 明确执行输入、ExecutionRuntime 完成收取/事件派发、项目通知、外层结构采用、
Root.update、再次结构采用、UI 场景输入与唯一 SceneRuntime.tick。没有额外 drawUi 或每工具推进循环。
等待判断包含已接纳的关闭/菜单/资产/布局意图；IO 和用户决定不会被当成可立即完成的工作反复尝试。

MenuItem 属于 function/ui，CommandRegistration 属于 Editor metadata；产品名单只在 executable 的
product/ProductAssembly.cpp 与 ProductCommands.cpp。Editor 库不包含具体工具。
菜单冻结对象目标、HistoryId、命令登记版本，执行时验证；程序发出的直接应用命令在宿主安全点采用。
File 提供项目创建/新进程打开、最近项目、资产浏览、新建、保存/另存为/全部保存、关闭与退出；
Edit 提供历史和文本命令、设置；Window 从登记与实际窗口生成，含布局选择；插件扩展自己的菜单项。
保存复用工具的 AssetActions 与原保存请求，任务 owner 保留成功、失败及重试事实。

工作区位于项目本机目录 `.lux/editor/layouts/<name>.toml`，选择记录在 `.lux/editor/settings.toml`。
version=1；每项包含顶层 Pane 类型、稳定 ID、可见状态、工厂的 opaque payload，以及完整 DockState。内部辅助 Pane 的角色 ID/可见性另行记录，工厂构造后恢复，不独立创建。
恢复复用已开的同资产工具，不覆盖脏内容、不关闭额外窗口；未保存新资产恢复为空工具。
未知或恢复失败的插件记录在再次保存时保留。Settings 的 Layouts 页支持保存、应用、重命名、删除及默认布局。
读写通过 ExecutionRuntime 的 blocking scheduler，临时文件 flush/close 后原子 rename；不走资产 VFS。
最近项目保存在平台用户配置目录的 `lux/editor/recent-projects.toml`，最多二十条；
项目验证/进程启动在后台，原生文件对话框仍在窗口所属线程。

本次公开菜单/恢复接口修改对应 Editor 插件 ABI v6；旧 ABI 不留兼容出口。
