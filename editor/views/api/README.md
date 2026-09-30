# View API（P08）

窄公共契约只链接 UI 和 contracts。ViewId 是 host 域、槽位和代次；恢复键独立；ViewTypeId 复用 PaneTypeId。
Host 为一次挂载分配 PaneId，不能把稳定恢复键当成 live ViewId。ViewInfo 拥有字符串，不暴露集合。

DetachedView 是 code + unique_ptr<Pane> 的唯一 owning 单元，移动赋值以完整单元交换，旧节点先于旧 code 清理。
必须离树构造；挂载后由 Host 保持该单元，完成卸载和资源责任交接再析构。直接析构仍挂载的单元是契约错误。
Pane 和控件成员／unique_ptr 各自只有一个 C++ owner；Root 和 LuxObject 父子链只观察。

ViewRequests 的 close/show/focus 接收 ViewId，宿主排队，在安全点重新检查代次。当前回调不得删除自身。
本阶段仅提供真实 Root 协议及测试用 FakeHost，不提供 P10 桌面宿主，不作 P12 产品切换。

`test/lifecycle.cpp` 同时用于安装消费者，执行真实 ImGui Root 的绘制、注册、焦点撤销、离树析构与通知故障。
这不是 P10/P13 GPU 像素、IME 或新产品资格。
