#pragma once

#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <type_traits>

namespace api_contract
{
    template <class T>
    concept PublicFreeze = requires(T& value) { value.freeze(); };
    template <class T>
    concept PublicFactoryReplacement = requires(T& value, lux::editor::UiFactory replacement) {
        value.resolveFactory("type")->get() = std::move(replacement);
    };
    template <class T>
    concept PublicRegistration = requires(T& value) { value.template registerFactory<int>({}); };
    template <class T>
    concept PublicServiceLookup = requires(T& value) { value.template service<int>(); };
    template <class T>
    concept PublicProfileLookup = requires(T& value) { value.find("test"); };
    template <class T>
    concept PublicProfileRegistration = requires(T& value) { value.registerProfile({}); };
    template <class T>
    concept PublicPaneOwners = requires(T& value) { value.panes(); };
    template <class T>
    concept PublicPendingDrain = requires(T& value) { value.applyPendingChanges(); };
    template <class T>
    concept PublicRuntimePaneId = requires(T& value) { value.id(); };

    static_assert(!PublicFreeze<lux::editor::EditorContext>);
    static_assert(!PublicFreeze<lux::editor::EditorServices>);
    static_assert(!PublicFreeze<lux::editor::EditorUiRegistry>);
    static_assert(!PublicFreeze<lux::editor::SceneToolRegistry>);
    static_assert(!PublicFreeze<lux::editor::SceneProfileRegistry>);
    static_assert(!PublicFactoryReplacement<lux::editor::EditorUiRegistry>);
} // namespace api_contract
