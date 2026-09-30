# 实际 SDK 回收回调测试草图

**状态：未在本环境编译／执行。不是运行证据，不是完整补丁。**

目的：使用已有 `editor/tools/material/interaction/test/interaction.cpp` 的真实 Fixture，不访问或篡改 SessionStore 私有状态，使 Store 按正常 close 协议进入 reclaiming；在另一个真实会话 B 的最后 code owner 清理中访问 A。

以下代码预期放进原匿名 namespace，复用已有 `Fixture`、`Saved`、`source()`、`take()` 及名称空间。需增加 `<functional>`、`<memory>` 等实际需要的头。原文件当前已经包含部分头；根据编译结果补齐，不使用隔离头。

```cpp
struct OnOtherSessionReclaimed final
{
    std::function<void()> notify;
    ~OnOtherSessionReclaimed() noexcept
    {
        if (notify)
            notify();
    }
};

bool anotherSessionReclaimsWhileInteractionIsLive(bool synchronize)
{
    Fixture f;
    const auto key = take(f.store.key<MaterialSession>(f.id));
    MaterialInteraction gesture(f.store.access<MaterialSession>(), key);
    const auto node = f.first();
    assert(gesture.select({node}));
    assert(gesture.begin("not yet committed"));
    std::vector<VMaterialEdit> preview;
    preview.push_back(MaterialPlaceNode{node, {50, 60}});
    assert(gesture.preview(preview));
    const Saved original(f);
    const auto start_stamp = gesture.overlay()->expected;

    bool callback_called = false;
    bool lookup_busy = false;
    bool accepted = false;
    bool reported_busy = false;
    bool overlay_retained = false;
    std::size_t selection_count = 0;

    auto hook = std::make_shared<OnOtherSessionReclaimed>();
    auto reservation = take(f.store.reserve<MaterialSession>(
        {"lux.editor.material"}, contracts::CodeLease::plugin(hook)));
    const auto other_id = reservation.id();
    auto input = source();
    auto binding = sessions::BoundSource{input.id, "other.material"};
    auto other = take(MaterialSession::create(
        other_id, std::move(binding), std::move(input)));
    assert(f.store.prepare(reservation, other));
    assert(f.store.publish(reservation));

    hook->notify = [&]() noexcept {
        callback_called = true;
        // 只通过实际公开 typed access 观察，不触碰 Store Impl。
        auto available = f.store.access<MaterialSession>().read(key);
        lookup_busy = !available && available.error() == sessions::ESessionError::BUSY;
        auto result = synchronize ? gesture.synchronize() : gesture.cancel();
        accepted = bool(result);
        reported_busy = !result && result.error().session == sessions::ESessionError::BUSY;
        overlay_retained = gesture.overlay() != nullptr;
        selection_count = gesture.selection().size();
    };
    hook.reset(); // B 槽中的 CodeLease 现在是这个 owner 的最后强引用。

    const auto other_info = take(f.store.describe(other_id));
    auto permit = take(f.store.prepareClose(other_info.current));
    assert(f.store.close(permit)); // notify 在真实 Slot 清理期间发生。
    assert(callback_called && lookup_busy);
    const auto still_live = f.store.describe(f.id);
    assert(still_live);
    original.unchanged(f);

    std::printf(
        "operation=%s callback=1 lookup_busy=%d target_still_live=%d result_ok=%d "
        "reported_busy=%d overlay_retained=%d selection_count=%zu\n",
        synchronize ? "synchronize" : "cancel", lookup_busy, bool(still_live),
        accepted, reported_busy, overlay_retained, selection_count);
    std::fflush(stdout);

    const bool correct = !accepted && reported_busy && overlay_retained && selection_count == 1;
    if (!correct)
        return false; // before 主程序明确退出 1，不能反向断言装成通过。

    assert(gesture.overlay()->expected == start_stamp);
    if (synchronize)
    {
        assert(gesture.synchronize()); // B 已退出回收后安全重试。
        assert(gesture.overlay() && gesture.selection().size() == 1);
        const auto before = f.history();
        assert(gesture.commit());
        assert(f.history().entry_count == before.entry_count + 1);
        assert(!gesture.overlay());
    }
    else
    {
        assert(gesture.cancel()); // 原显式取消可在安全点成功。
        assert(!gesture.overlay() && gesture.selection().size() == 1);
        original.unchanged(f);
    }
    return true;
}
```

## 接线注意

- 这不是“真实 DLL 卸载”。普通 shared code owner 只负责在真实 Store 回收边界调用通知，不需新增插件系统。
- 对 `synchronize=false` 和 `true` 使用 fresh fixture，避免第一个失败已经清空状态影响第二个结果。
- before 可增加命令行 mode 返回 `correct ? 0 : 1`，保留实际 stdout／exit code。
- after 继续在原 target 使用同一场景与同一判定；不能换成注入 BUSY 的 fake。
- Source A 对 Scene／Flow 的版本复用各自原 Fixture，并检查其领域批次和引用；B 可以仍是同一 Store 中的真实 Material 会话。若跨模型 fixture 接线不便，各自 B 使用相同作者类型亦可。
- 安装消费者不得 include model/src 私有访问头。`Saved` 的全量私有 History 检查可留 native 测试；安装侧通过公开描述、冻结编码、公开 Undo/Redo 和后续 Commit 验证同一行为。
- 第三个必测变体：A 只有选择、没有 begin。synchronize 在 BUSY 时也必须保留选择，这能防止只修改 cancel 的不完整补正。
- 必须同时保留目标 A 真关闭、旧 key 新 generation、读取 gate BUSY、正常取消与无手势幂等对照。
