#pragma once

#include <lux/engine/editor/EditorContext.hpp>
#include <type_traits>

namespace api_contract
{
    template <class T>
    concept PublicFreeze = requires(T& value) { value.freeze(); };
    template <class T>
    concept PublicFactoryReplacement = requires(T& value) { value.resolveFactory("type"); };
    template <class T>
    concept PublicPaneOwners = requires(T& value) { value.panes(); };
    template <class T>
    concept PublicPendingDrain = requires(T& value) { value.applyPendingChanges(); };
    template <class T>
    concept PublicRuntimePaneId = requires(T& value) { value.id(); };

    static_assert(!PublicFreeze<lux::editor::EditorContext>);
    static_assert(!PublicFreeze<lux::editor::EditorServiceRegistrar>);
    static_assert(!PublicFreeze<lux::editor::EditorUiRegistrar>);
    static_assert(!PublicFreeze<lux::editor::SceneToolRegistrar>);
    static_assert(!PublicFreeze<lux::editor::SceneProfileRegistrar>);
    static_assert(!PublicFactoryReplacement<lux::editor::EditorUiRegistrar>);
} // namespace api_contract
