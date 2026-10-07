#pragma once

#include <cassert>
#include <cstddef>
#include <iostream>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <type_traits>

namespace lifetime_checks
{
    using namespace lux;
    template <class T>
    concept GenericParent = requires(T& v, object::LuxObject& p) { v.setParent(&p); };
    template <class T>
    concept GenericAdd = requires(T& v, object::LuxObject& p) { v.addChild(p); };
    template <class T>
    concept GenericRemove = requires(T& v, object::LuxObject& p) { v.removeChild(p); };
    template <class T>
    concept StructureProbe = requires(const T& v) { v.checkStructureSafe(); };
    static_assert(!GenericParent<ui::Root> && !GenericParent<ui::Pane> && !GenericParent<ui::Element>);
    static_assert(!GenericAdd<ui::Root> && !GenericAdd<ui::Pane> && !GenericAdd<ui::Element>);
    static_assert(!GenericRemove<ui::Root> && !GenericRemove<ui::Pane> && !GenericRemove<ui::Element>);
    static_assert(!StructureProbe<ui::Root>);
    static_assert(!std::is_copy_constructible_v<ui::PaneChanged>);
    static_assert(!std::is_copy_constructible_v<ui::ObjectRemoved>);
    static_assert(std::is_copy_constructible_v<ui::DockTree> && std::is_copy_constructible_v<ui::PaneHandle>);

    alignas(std::max_align_t) inline std::byte storage[4096];
    class ReusedPane final : public ui::Pane
    {
    public:
        ReusedPane() : Pane("Reused address") {}
        static void* operator new(std::size_t n)
        {
            assert(n <= sizeof(storage));
            return storage;
        }
        static void operator delete(void*) noexcept {}
    };

    inline void run()
    {
        auto made = ui::Root::create();
        assert(made);
        auto root = std::move(*made);
        auto first = root->addPane(std::unique_ptr<ui::Pane>{new ReusedPane});
        assert(first);
        auto* address = &first->get();
        const auto handle = root->paneHandle(*address);
        const auto object_id = address->objectId();
        assert(!handle.isNull() && root->resolvePane(handle) == address);
        auto tree = root->captureDockTree();
        assert(root->setDockTree(tree));
        unsigned removals{}, detached{};
        object::LuxObject receiver;
        auto removed = object::LuxObject::connect(
            root.get(),
            &ui::Root::objectRemoved,
            &receiver,
            [&](const ui::ObjectRemoved& notification) noexcept
            {
                assert(notification.object == address);
                assert(!root->resolvePane(handle) && !root->focusedPane());
                assert(notification.object->objectId() == object_id);
                assert(!root->clearPanes());
                ++removals;
            }
        );
        auto changed = object::LuxObject::connect(
            root.get(),
            &ui::Root::paneChanged,
            &receiver,
            [&](const ui::PaneChanged& notification) noexcept
            {
                if (!notification.attached)
                {
                    assert(notification.pane == address && !root->resolvePane(handle));
                    ++detached;
                }
            }
        );
        auto queued_removed = object::LuxObject::connect(
            root.get(),
            &ui::Root::objectRemoved,
            &receiver,
            [](const ui::ObjectRemoved&) noexcept { std::abort(); },
            object::EDelivery::QUEUED
        );
        auto queued_changed = object::LuxObject::connect(
            root.get(),
            &ui::Root::paneChanged,
            &receiver,
            [](const ui::PaneChanged&) noexcept { std::abort(); },
            object::EDelivery::QUEUED
        );
        assert(!queued_removed && queued_removed.error() == object::EConnectError::PAYLOAD_NOT_QUEUEABLE);
        assert(!queued_changed && queued_changed.error() == object::EConnectError::PAYLOAD_NOT_QUEUEABLE);
        assert(removed && changed && root->clearPanes() && removals == 1 && detached == 1);
        removed->disconnect();
        changed->disconnect();
        assert(!object::ObjectRuntime::instance().resolve(object_id));
        assert(!root->resolvePane(handle));
        // A formerly captured layout is rejected even before its old address is reused.
        assert(!root->setDockTree(tree));
        auto second = root->addPane(std::unique_ptr<ui::Pane>{new ReusedPane});
        assert(second && &second->get() == address && second->get().objectId() != object_id);
        assert(!root->setDockTree(tree));
        auto current = root->paneHandle(second->get());
        assert(current != handle && root->resolvePane(current) == address);
        auto owner = root->removePane(second->get());
        assert(owner && !root->resolvePane(current));
        assert(root->addPane(std::move(*owner)));
        assert(root->paneHandle(*address) != current && !root->resolvePane(current));
        auto valid = ui::DockTree{
            {{ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {root->paneHandle(*address)}}},
            {{0, {{0, 0}, {640, 480}}, false}}
        };
        assert(root->setDockTree(valid));
        auto replacement = ui::Root::create();
        assert(replacement && !(*replacement)->setDockTree(valid));
        owner = root->removePane(*address);
        assert(owner);
        root.reset();
        assert((*replacement)->addPane(std::move(*owner)));
        assert(!(*replacement)->resolvePane(current) && !(*replacement)->setDockTree(valid));
        assert((*replacement)->setDockTree((*replacement)->captureDockTree()));
        static_cast<void>(object::ObjectRuntime::instance().dispatchPending());
        std::cout << "PASS stale layouts, remount/cross-Root identity, address reuse and nonqueueable borrows\n";
    }
} // namespace lifetime_checks
