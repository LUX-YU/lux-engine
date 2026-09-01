#pragma once

#include <lux/engine/system/SystemInstanceId.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/EventPoint.hpp>
#include <lux/engine/simulation/SimulationEndpointSpec.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>

namespace lux::simulation::script
{
    using ScriptHookLane = void (*)(void *, lux_script_call_frame &) noexcept;
    using ScriptEventLane = void (*)(void *, ecs::Entity, lux_script_call_frame &) noexcept;

    struct ScriptHookEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        HookPointId hook;
        lux::semantic::SignatureView signature;
        void *context{};
        EndpointConnectResult (*connect)(void *, void *, ScriptHookLane) noexcept {};
        EEndpointMutationError (*disconnect)(void *, EndpointConnectionToken) noexcept {};
    };

    struct ScriptEventEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        EventPointId event;
        EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
        lux::semantic::Type payload_type;
        void *context{};
        EndpointConnectResult (*connect)(void *, void *, ScriptEventLane) noexcept {};
        EEndpointMutationError (*disconnect)(void *, EndpointConnectionToken) noexcept {};
    };

    namespace detail
    {
        template <class Parameter>
        [[nodiscard]] lux_script_value_slot argumentSlot(Parameter &value) noexcept
        {
            using Base = std::remove_cv_t<std::remove_reference_t<Parameter>>;
            using Traits = lux::semantic::TypeTraits<Base>;
            return lux_script_value_slot{
                Traits::AbiKind,
                {},
                Traits::Size,
                lux::semantic::typeId(Traits::CanonicalName),
                const_cast<void *>(static_cast<const void *>(std::addressof(value)))};
        }

        template <class... Parameters>
        [[nodiscard]] auto argumentSlots(Parameters &...parameters) noexcept
        {
            return std::array<lux_script_value_slot, sizeof...(Parameters)>{
                argumentSlot(parameters)...};
        }
    }

    template <class Signature>
    class ScriptHookEndpoint;

    template <class... Parameters>
    class ScriptHookEndpoint<void(Parameters...)>
    {
    public:
        ScriptHookEndpoint(
            lux::system::SystemInstanceId system,
            HookPointId id,
            HookPoint<void(Parameters...)> &endpoint) noexcept
            : system_(system), id_(id), endpoint_(&endpoint)
        {
        }

        ScriptHookEndpoint(const ScriptHookEndpoint &) = delete;
        ScriptHookEndpoint &operator=(const ScriptHookEndpoint &) = delete;
        ScriptHookEndpoint(ScriptHookEndpoint &&) = delete;
        ScriptHookEndpoint &operator=(ScriptHookEndpoint &&) = delete;

        [[nodiscard]] ScriptHookEndpointDescriptor descriptor() noexcept
        {
            return {
                system_,
                id_,
                lux::simulation::detail::EndpointSignatureStorage<void(Parameters...)>::view(),
                this,
                &connect,
                &disconnect
            };
        }

    private:
        static EndpointConnectResult connect(
            void *context,
            void *lane_context,
            ScriptHookLane lane) noexcept
        {
            auto &self = *static_cast<ScriptHookEndpoint *>(context);
            self.lane_context_ = lane_context;
            self.lane_ = lane;
            return self.endpoint_->connect(&self, &dispatch);
        }

        static EEndpointMutationError disconnect(
            void *context,
            EndpointConnectionToken token) noexcept
        {
            return static_cast<ScriptHookEndpoint *>(context)
                ->endpoint_->disconnect(token);
        }

        static void dispatch(
            void *context,
            Parameters... parameters) noexcept
        {
            auto &self = *static_cast<ScriptHookEndpoint *>(context);
            auto slots = detail::argumentSlots(parameters...);
            lux_script_call_frame frame{
                slots.data(),
                static_cast<std::uint32_t>(slots.size()),
                0U,
                nullptr,
                0U,
                0U,
                nullptr,
                nullptr};
            self.lane_(self.lane_context_, frame);
        }

        lux::system::SystemInstanceId system_;
        HookPointId id_;
        HookPoint<void(Parameters...)> *endpoint_{};
        void *lane_context_{};
        ScriptHookLane lane_{};
    };

    template <class... Parameters>
    class ScriptHookEndpoint<void(Parameters...) noexcept> final
        : public ScriptHookEndpoint<void(Parameters...)>
    {
        using Base = ScriptHookEndpoint<void(Parameters...)>;

    public:
        using Base::Base;
    };

    template <class Route, class Payload>
    class ScriptEventEndpoint;

    template <class Payload>
    class ScriptEventEndpoint<SimulationBroadcastRoute, Payload> final
    {
    public:
        ScriptEventEndpoint(
            lux::system::SystemInstanceId system,
            EventPointId id,
            EventPoint<SimulationBroadcastRoute, Payload> &endpoint) noexcept
            : system_(system), id_(id), endpoint_(&endpoint)
        {
        }

        ScriptEventEndpoint(const ScriptEventEndpoint &) = delete;
        ScriptEventEndpoint &operator=(const ScriptEventEndpoint &) = delete;
        ScriptEventEndpoint(ScriptEventEndpoint &&) = delete;
        ScriptEventEndpoint &operator=(ScriptEventEndpoint &&) = delete;

        [[nodiscard]] ScriptEventEndpointDescriptor descriptor() noexcept
        {
            return {
                system_,
                id_,
                EEventRoute::SIMULATION_BROADCAST,
                lux::semantic::makeType<Payload>(
                    lux::semantic::EValuePass::CONST_REF),
                this,
                &connect,
                &disconnect};
        }

    private:
        static EndpointConnectResult connect(
            void *context,
            void *lane_context,
            ScriptEventLane lane) noexcept
        {
            auto &self = *static_cast<ScriptEventEndpoint *>(context);
            self.lane_context_ = lane_context;
            self.lane_ = lane;
            return self.endpoint_->connect(&self, &dispatch);
        }

        static EEndpointMutationError disconnect(
            void *context,
            EndpointConnectionToken token) noexcept
        {
            return static_cast<ScriptEventEndpoint *>(context)
                ->endpoint_->disconnect(token);
        }

        static void dispatch(void *context, const Payload &payload) noexcept
        {
            auto &self = *static_cast<ScriptEventEndpoint *>(context);
            auto slot = detail::argumentSlot(payload);
            lux_script_call_frame frame{
                &slot,
                1U,
                0U,
                nullptr,
                0U,
                0U,
                nullptr,
                nullptr};
            self.lane_(self.lane_context_, ecs::NullEntity, frame);
        }

        lux::system::SystemInstanceId system_;
        EventPointId id_;
        EventPoint<SimulationBroadcastRoute, Payload> *endpoint_{};
        void *lane_context_{};
        ScriptEventLane lane_{};
    };

    template <class Payload>
    class ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, Payload> final
    {
    public:
        ScriptEventEndpoint(
            lux::system::SystemInstanceId system,
            EventPointId id,
            EventPoint<EntityTargetedRoute<ecs::Entity>, Payload> &endpoint) noexcept
            : system_(system), id_(id), endpoint_(&endpoint)
        {
        }

        ScriptEventEndpoint(const ScriptEventEndpoint &) = delete;
        ScriptEventEndpoint &operator=(const ScriptEventEndpoint &) = delete;
        ScriptEventEndpoint(ScriptEventEndpoint &&) = delete;
        ScriptEventEndpoint &operator=(ScriptEventEndpoint &&) = delete;

        [[nodiscard]] ScriptEventEndpointDescriptor descriptor() noexcept
        {
            return {
                system_,
                id_,
                EEventRoute::ENTITY_TARGETED,
                lux::semantic::makeType<Payload>(
                    lux::semantic::EValuePass::CONST_REF),
                this,
                &connect,
                &disconnect};
        }

    private:
        static EndpointConnectResult connect(
            void *context,
            void *lane_context,
            ScriptEventLane lane) noexcept
        {
            auto &self = *static_cast<ScriptEventEndpoint *>(context);
            self.lane_context_ = lane_context;
            self.lane_ = lane;
            return self.endpoint_->connectAll(&self, &dispatch);
        }

        static EEndpointMutationError disconnect(
            void *context,
            EndpointConnectionToken token) noexcept
        {
            return static_cast<ScriptEventEndpoint *>(context)
                ->endpoint_->disconnect(token);
        }

        static void dispatch(
            void *context,
            const ecs::Entity &target,
            const Payload &payload) noexcept
        {
            auto &self = *static_cast<ScriptEventEndpoint *>(context);
            auto slot = detail::argumentSlot(payload);
            lux_script_call_frame frame{
                &slot,
                1U,
                0U,
                nullptr,
                0U,
                0U,
                nullptr,
                nullptr};
            self.lane_(self.lane_context_, target, frame);
        }

        lux::system::SystemInstanceId system_;
        EventPointId id_;
        EventPoint<EntityTargetedRoute<ecs::Entity>, Payload> *endpoint_{};
        void *lane_context_{};
        ScriptEventLane lane_{};
    };
}
