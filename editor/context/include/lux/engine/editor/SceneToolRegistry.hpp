#pragma once
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
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
    class SceneToolRegistry final
    {
    public:
        using Match = bool (*)(const world::WorldDescription&) noexcept;
        SceneToolRegistry(const SceneToolRegistry&) = delete;
        SceneToolRegistry& operator=(const SceneToolRegistry&) = delete;
        SceneToolRegistry(SceneToolRegistry&&) = delete;
        SceneToolRegistry& operator=(SceneToolRegistry&&) = delete;

        template <class T>
        [[nodiscard]] FrameworkResult<std::unique_ptr<T>> create(
            EditorContext& context,
            const world::WorldDescription& world
        ) const noexcept
        {
            const Entry* selected{};
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
        friend class EditorComposition;
        using Owner = std::unique_ptr<void, void (*)(void*) noexcept>;

        struct Entry final
        {
            cxx::TypeToken type;
            Match match;
            mutable cxx::move_only_function<
                FrameworkResult<Owner>(EditorContext&, const world::WorldDescription&) noexcept>
                factory;
        };

        std::vector<Entry> entries_;
        explicit SceneToolRegistry(std::vector<Entry>) noexcept;
    };
} // namespace lux::editor
