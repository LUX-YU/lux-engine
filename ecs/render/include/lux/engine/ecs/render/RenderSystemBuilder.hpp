#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/function/visibility.h>

#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class ERenderAssemblyError : std::uint8_t
    {
        NullSubsystem,
        DuplicateType,
        MissingPrerequisite,
        TopologyCycle,
        BuilderFrozen,
        TypeCollision,
        MutationUnavailable,
        InvalidHandle,
        RemovalUnsupported,
        RequiredByOtherSubsystem,
        EmptyBatch,
        PlanClosed,
    };

    struct RenderAssemblyFailure
    {
        ERenderAssemblyError code{ERenderAssemblyError::NullSubsystem};
        RenderSubsystemType  subject{};
        RenderSubsystemType  related{};
    };

    class RenderSystemBuilder;
    class RenderSubsystemMutationBatch;

    struct RenderSubsystemHandleAny final
    {
        std::uint32_t slot{0xFFFFFFFFu};
        std::uint32_t generation{0u};
        RenderSubsystemType type{};
    };

    struct InstalledRenderSubsystemBatch final
    {
        InstalledRenderSubsystemBatch() = default;
        InstalledRenderSubsystemBatch(
            const InstalledRenderSubsystemBatch&) = delete;
        InstalledRenderSubsystemBatch& operator=(
            const InstalledRenderSubsystemBatch&) = delete;
        InstalledRenderSubsystemBatch(
            InstalledRenderSubsystemBatch&&) noexcept = default;
        InstalledRenderSubsystemBatch& operator=(
            InstalledRenderSubsystemBatch&&) noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return plan_identity != 0u && !handles.empty();
        }

        std::uint64_t plan_identity{0u};
        std::vector<RenderSubsystemHandleAny> handles;
    };

    class LUX_FUNCTION_PUBLIC RenderSubsystemMutationBatch final
    {
    public:
        RenderSubsystemMutationBatch();
        ~RenderSubsystemMutationBatch();
        RenderSubsystemMutationBatch(const RenderSubsystemMutationBatch&) =
            delete;
        RenderSubsystemMutationBatch& operator=(
            const RenderSubsystemMutationBatch&) = delete;
        RenderSubsystemMutationBatch(
            RenderSubsystemMutationBatch&&) noexcept;
        RenderSubsystemMutationBatch& operator=(
            RenderSubsystemMutationBatch&&) noexcept;

        template <class Subsystem>
            requires std::derived_from<Subsystem, IRenderSubsystem> &&
                     (!std::same_as<Subsystem, IRenderSubsystem>)
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure> add(
            std::unique_ptr<Subsystem> subsystem,
            std::string_view diagnostic_name = typeToken<Subsystem>().name)
        {
            return addErased(
                std::move(subsystem),
                renderSubsystemType<Subsystem>(),
                diagnostic_name);
        }

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        friend class RenderSystemPlan;
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure>
        addErased(
            std::unique_ptr<IRenderSubsystem> subsystem,
            RenderSubsystemType type,
            std::string_view diagnostic_name);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// Move-only extraction graph owner. Runtime work is an immutable linear
    /// pointer plan; graph mutation only happens through atomic safe-point
    /// batches.
    class LUX_FUNCTION_PUBLIC RenderSystemPlan final
    {
    public:
        RenderSystemPlan();
        ~RenderSystemPlan();

        RenderSystemPlan(const RenderSystemPlan&) = delete;
        RenderSystemPlan& operator=(const RenderSystemPlan&) = delete;
        RenderSystemPlan(RenderSystemPlan&&) noexcept;
        RenderSystemPlan& operator=(RenderSystemPlan&&) noexcept;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::span<const std::string_view> features() const noexcept;
        [[nodiscard]] std::uint64_t droppedStaleCommands() const noexcept;

    private:
        friend class RenderSystemBuilder;
        friend class RenderSystem;
        void activate(lux::meta::EntityRegistry& registry);
        void update(
            lux::meta::EntityRegistry& registry,
            SceneRenderBinding&        render,
            ActiveRenderView&          active_view,
            float                      dt,
            std::uint64_t              tick_index
        );
        void settle(
            lux::meta::EntityRegistry& registry,
            SceneRenderBinding&        render,
            ActiveRenderView&          active_view,
            std::uint64_t              tick_index
        );
        void close(
            lux::meta::EntityRegistry& registry,
            SceneRenderBinding&        render,
            ActiveRenderView&          active_view,
            std::uint64_t              tick_index
        ) noexcept;
        void detach(lux::meta::EntityRegistry& registry) noexcept;
        [[nodiscard]] lux::cxx::expected<
            InstalledRenderSubsystemBatch,
            RenderAssemblyFailure>
        installBatch(
            RenderSubsystemMutationBatch&& batch,
            SceneRenderBinding& render,
            ActiveRenderView& active_view,
            std::uint64_t tick_index);
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure>
        removeBatch(
            InstalledRenderSubsystemBatch&& batch,
            SceneRenderBinding& render,
            ActiveRenderView& active_view,
            std::uint64_t tick_index);
        struct Impl;
        explicit RenderSystemPlan(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    class LUX_FUNCTION_PUBLIC RenderSystemBuilder final
    {
    public:
        RenderSystemBuilder();
        ~RenderSystemBuilder();

        RenderSystemBuilder(const RenderSystemBuilder&) = delete;
        RenderSystemBuilder& operator=(const RenderSystemBuilder&) = delete;
        RenderSystemBuilder(RenderSystemBuilder&&) noexcept;
        RenderSystemBuilder& operator=(RenderSystemBuilder&&) noexcept;

        template <class Subsystem>
            requires std::derived_from<Subsystem, IRenderSubsystem> &&
                     (!std::same_as<Subsystem, IRenderSubsystem>)
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure> add(
            std::unique_ptr<Subsystem> subsystem,
            std::string_view diagnostic_name = typeToken<Subsystem>().name)
        {
            return addErased(
                std::move(subsystem),
                renderSubsystemType<Subsystem>(),
                diagnostic_name
            );
        }

        [[nodiscard]] lux::cxx::expected<
            RenderSystemPlan,
            RenderAssemblyFailure>
        compile() &&;

    private:
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure>
        addErased(
            std::unique_ptr<IRenderSubsystem> subsystem,
            RenderSubsystemType              type,
            std::string_view                  diagnostic_name
        );

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ecs
