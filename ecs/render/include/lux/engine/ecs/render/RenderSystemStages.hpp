#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/function/visibility.h>

#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::ecs
{
    enum class ERenderStageAssemblyError : std::uint8_t
    {
        NullStage,
        DuplicateType,
        TypeCollision,
        Frozen,
    };

    struct RenderStageAssemblyFailure final
    {
        ERenderStageAssemblyError code{ERenderStageAssemblyError::NullStage};
        lux::cxx::TypeToken subject{};
        lux::cxx::TypeToken related{};
    };

    /// Cold-path, append-only owner for RenderSystem's immutable extraction
    /// sequence. This is deliberately not a graph: it accepts no dependency
    /// declarations and cannot be mutated after freeze().
    class LUX_FUNCTION_PUBLIC RenderSystemStages final
    {
    public:
        RenderSystemStages();
        ~RenderSystemStages();
        RenderSystemStages(const RenderSystemStages&) = delete;
        RenderSystemStages& operator=(const RenderSystemStages&) = delete;
        RenderSystemStages(RenderSystemStages&&) noexcept;
        RenderSystemStages& operator=(RenderSystemStages&&) noexcept;

        template <class Stage>
            requires std::derived_from<Stage, RenderStage> &&
                     (!std::same_as<Stage, RenderStage>)
        [[nodiscard]] lux::cxx::expected<void, RenderStageAssemblyFailure>
        add(std::unique_ptr<Stage> stage)
        {
            return addErased(
                std::move(stage),
                lux::cxx::typeToken<Stage>()
            );
        }

        [[nodiscard]] lux::cxx::expected<void, RenderStageAssemblyFailure>
        freeze() noexcept;
        [[nodiscard]] bool frozen() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept;

    private:
        friend class RenderSystem;
        [[nodiscard]] lux::cxx::expected<void, RenderStageAssemblyFailure>
        addErased(
            std::unique_ptr<RenderStage> stage,
            lux::cxx::TypeToken type
        );
        void activate(lux::ecs::Registry& registry);
        void extract(
            lux::ecs::Registry& registry,
            SceneRenderBinding& render,
            ActiveRenderView& active_view,
            float dt,
            std::uint64_t tick_index
        );
        void settle(
            lux::ecs::Registry& registry,
            SceneRenderBinding& render,
            ActiveRenderView& active_view,
            std::uint64_t tick_index
        );
        void close(
            lux::ecs::Registry& registry,
            SceneRenderBinding& render,
            ActiveRenderView& active_view,
            std::uint64_t tick_index
        ) noexcept;
        void detach(lux::ecs::Registry& registry) noexcept;
        [[nodiscard]] std::uint64_t droppedStaleCommands() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
