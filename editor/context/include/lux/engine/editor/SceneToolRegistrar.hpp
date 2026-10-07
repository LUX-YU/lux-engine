#pragma once
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <memory>
#include <vector>

namespace lux::world
{
    struct WorldDescription;
}
namespace lux::editor
{
    class EditorContext;
    // Provisional: selection semantics are excluded from the Framework v2 freeze contract.
    // Revisit with the first real SceneSession/SceneToolSet rather than extending dummy rules.
    class SceneToolRegistrar final
    {
    public:
        using Match = bool (*)(const world::WorldDescription&) noexcept;
        SceneToolRegistrar() = default;
        SceneToolRegistrar(const SceneToolRegistrar&) = delete;
        SceneToolRegistrar& operator=(const SceneToolRegistrar&) = delete;
        SceneToolRegistrar(SceneToolRegistrar&&) = delete;
        SceneToolRegistrar& operator=(SceneToolRegistrar&&) = delete;

        template <class T>
        [[nodiscard]] FrameworkResult<void> registerFactory(
            Match match,
            cxx::move_only_function<
                FrameworkResult<std::unique_ptr<T>>(EditorContext&, const world::WorldDescription&) noexcept> factory
        ) noexcept
        {
            if (frozen_)
            {
                return cxx::unexpected(error::Error{Errors::EditorSceneToolRegistrationIsFrozen, {}});
            }
            const bool is_invalid_factory = match == nullptr || !factory;
            if (is_invalid_factory)
            {
                return cxx::unexpected(error::Error{Errors::EditorInvalidSceneToolFactory, {}});
            }
            for (const auto& entry : entries_)
            {
                const bool is_duplicate = entry.type == cxx::typeToken<T>() && entry.match == match;
                if (is_duplicate)
                {
                    return cxx::unexpected(error::Error{Errors::EditorDuplicateSceneToolRule, {}});
                }
            }
            using ErasedResult = FrameworkResult<Owner>;
            auto erased = [factory = std::move(factory)](
                EditorContext& context,
                const world::WorldDescription& world
            ) mutable noexcept -> ErasedResult
            {
                auto result = factory(context, world);
                if (!result)
                {
                    return cxx::unexpected(std::move(result.error()));
                }
                if (!*result)
                {
                    return cxx::unexpected(error::Error{Errors::EditorNullSceneToolSet, {}});
                }
                return Owner{result->release(), [](void* value) noexcept { delete static_cast<T*>(value); }};
            };
            entries_.push_back({cxx::typeToken<T>(), match, std::move(erased)});
            return {};
        }

        template <class T>
        [[nodiscard]] FrameworkResult<std::unique_ptr<T>> create(
            EditorContext& context,
            const world::WorldDescription& world
        ) noexcept
        {
            if (!frozen_)
            {
                return cxx::unexpected(error::Error{Errors::EditorSceneToolRegistrationIsNotFrozen, {}});
            }
            Entry* selected{};
            for (auto& entry : entries_)
            {
                if (entry.type != cxx::typeToken<T>() || !entry.match(world))
                {
                    continue;
                }
                if (selected)
                {
                    return cxx::unexpected(error::Error{Errors::EditorAmbiguousSceneToolRules, {}});
                }
                selected = &entry;
            }
            if (!selected)
            {
                return cxx::unexpected(error::Error{Errors::EditorNoMatchingSceneToolRule, {}});
            }
            auto result = selected->factory(context, world);
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::unique_ptr<T>{static_cast<T*>(result->release())};
        }

    private:
        friend class EditorContext;
        void freeze() noexcept
        {
            frozen_ = true;
        }
        using Owner = std::unique_ptr<void, void (*)(void*) noexcept>;
        struct Entry final
        {
            cxx::TypeToken type;
            Match match;
            cxx::move_only_function<FrameworkResult<Owner>(EditorContext&, const world::WorldDescription&) noexcept>
                factory;
        };
        std::vector<Entry> entries_;
        bool frozen_{};
    };
} // namespace lux::editor
