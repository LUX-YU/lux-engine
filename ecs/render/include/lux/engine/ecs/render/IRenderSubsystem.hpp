#pragma once

#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/SystemUpdateContext.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/ecs/render/RenderSubsystemContext.hpp>
#include <lux/engine/function/visibility.h>

#include <span>
#include <string_view>

namespace lux::ecs
{
    using RenderSubsystemType = lux::cxx::TypeToken;

    template <class Subsystem>
    [[nodiscard]] constexpr RenderSubsystemType renderSubsystemType() noexcept
    {
        return lux::cxx::typeToken<Subsystem>();
    }

    /// One ECS -> render extraction stage owned by RenderSystem. Runtime
    /// mutation is a RenderSystem safe-point concern; the subsystem itself is
    /// not an ISystem and never becomes a second engine scheduler node.
    class LUX_FUNCTION_PUBLIC IRenderSubsystem : public IEcsCommandProducer
    {
    public:
        virtual ~IRenderSubsystem() = default;

        IRenderSubsystem(const IRenderSubsystem&) = delete;
        IRenderSubsystem& operator=(const IRenderSubsystem&) = delete;

        virtual void onAdded(const SystemSetupContext&) {}
        virtual void onRemoved(const SystemRemovalContext&) {}

        [[nodiscard]] virtual std::span<const RenderSubsystemType>
        prerequisites() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const RenderSubsystemType>
        runsAfter() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const RenderSubsystemType>
        runsBefore() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual std::span<const std::string_view>
        renderFeatures() const noexcept
        {
            return {};
        }

        /// Refresh non-owning frame/scene borrows before deferred commands are
        /// applied. This lets bring-up settle flush observer backfill first.
        virtual void prepare(RenderSubsystemContext&) noexcept {}

        virtual void update(RenderSubsystemContext& context) = 0;

        /// Dynamic removal at a RenderSystem frame safe point. Frame-lane
        /// cleanup is permitted here; destructors remain passive backstops.
        virtual void close(RenderSubsystemContext&) noexcept {}

        /// Whole-scene shutdown after the final lexical frame has closed.
        /// Implementations must not emit Frame-lane commands here. The default
        /// is for local/Control/Upload-only cleanup; frame emitters override it
        /// and discard only their scene-owned local cache.
        virtual void closeScene(RenderSubsystemContext& context) noexcept
        {
            close(context);
        }

        /// Bring-up settle hook used by asynchronous view creation. The
        /// lexical frame is closed while this hook runs: implementations may
        /// reconcile Control-lane state, but must not emit Frame-lane ops.
        /// Most extraction stages therefore have no work here.
        virtual void settle(RenderSubsystemContext&) {}

        [[nodiscard]] virtual bool supportsDynamicRemoval() const noexcept
        {
            return false;
        }

    protected:
        IRenderSubsystem() = default;
    };

} // namespace lux::ecs
