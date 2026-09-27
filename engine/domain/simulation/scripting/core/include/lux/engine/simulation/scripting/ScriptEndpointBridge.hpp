#pragma once

#include <lux/engine/system/SystemInstanceId.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/HookChannel.hpp>
#include <lux/engine/simulation/SimulationEndpointSpec.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

namespace lux::simulation::script
{
    using ScriptHookLane = void (*)(void*, lux_script_call_frame&) noexcept;
    using ScriptEventLane = void (*)(void*, ecs::Entity, lux_script_call_frame&) noexcept;

    template <class Route, class Payload> class TScriptEventEndpoint;

    struct ScriptEventPayloadProjection final
    {
        using Copy = bool (*)(void*, const lux_script_value_slot&, std::span<std::byte>) noexcept;
        ScriptEventPayloadProjection() noexcept = default;
        ScriptEventPayloadProjection(lux::semantic::Layout layout, Copy projection) noexcept
            : owned_layout(layout), copy(projection)
        {}
        [[nodiscard]] bool mayReenter() const noexcept
        {
            return non_reentrant_copy_ == nullptr || copy != non_reentrant_copy_;
        }
        lux::semantic::Layout owned_layout;
        Copy copy{};

    private:
        template <class Route, class Payload> friend class TScriptEventEndpoint;
        // A callback identity proved by the typed endpoint factory. Replacing the
        // public copy callback invalidates this fact automatically.
        Copy non_reentrant_copy_{};
    };

    struct ScriptHookEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        HookPointId hook;
        lux::semantic::SignatureView signature;
        void* context{};
        EndpointConnectResult (*connect)(void*, void*, ScriptHookLane) noexcept {};
        EEndpointMutationError (*disconnect)(void*, EndpointConnectionToken) noexcept {};
        void (*bind_owner)(void*, const void*) noexcept {};
        bool (*connected)(void*) noexcept {};
    };

    struct ScriptEventEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        EventPointId event;
        EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
        lux::semantic::Type payload_type;
        ScriptEventPayloadProjection payload_projection;
        void* context{};
        EndpointConnectResult (*connect)(void*, void*, ScriptEventLane) noexcept {};
        EEndpointMutationError (*disconnect)(void*, EndpointConnectionToken) noexcept {};
        std::size_t (*consume)(void*) noexcept {};
        bool (*failed)(void*) noexcept {};
        void* channel_context{};
        bool (*connected)(void*) noexcept {};
    };

    namespace detail
    {
        template <class Parameter> [[nodiscard]] lux_script_value_slot argumentSlot(Parameter& value) noexcept
        {
            using Base = std::remove_cv_t<std::remove_reference_t<Parameter>>;
            using Traits = lux::semantic::TTypeTraits<Base>;
            return lux_script_value_slot{
                Traits::AbiKind,
                {},
                Traits::Size,
                lux::semantic::typeId(Traits::CanonicalName),
                const_cast<void*>(static_cast<const void*>(std::addressof(value)))
            };
        }

        template <class... Parameters> [[nodiscard]] auto argumentSlots(Parameters&... parameters) noexcept
        {
            return std::array<lux_script_value_slot, sizeof...(Parameters)>{argumentSlot(parameters)...};
        }

        template <class Payload> using EventPayloadCopy = bool (*)(const Payload&, std::span<std::byte>) noexcept;

        template <class Payload>
        [[nodiscard]] bool copyScalarEventPayload(const Payload& payload, std::span<std::byte> output) noexcept
        {
            if (output.size() != sizeof(Payload))
                return false;
            std::memcpy(output.data(), std::addressof(payload), sizeof(Payload));
            return true;
        }

        template <class Payload> [[nodiscard]] constexpr EventPayloadCopy<Payload> defaultEventPayloadCopy() noexcept
        {
            constexpr auto kind = lux::semantic::TTypeTraits<std::remove_cv_t<Payload>>::AbiKind;
            constexpr bool is_scalar = kind >= static_cast<std::uint8_t>(lux::semantic::EAbiKind::BOOL) &&
                                       kind <= static_cast<std::uint8_t>(lux::semantic::EAbiKind::F64);
            if constexpr (is_scalar && std::is_trivially_copyable_v<Payload>)
                return &copyScalarEventPayload<Payload>;
            return nullptr;
        }

        template <class Payload> [[nodiscard]] constexpr lux::semantic::Layout eventPayloadLayout() noexcept
        {
            using Traits = lux::semantic::TTypeTraits<std::remove_cv_t<Payload>>;
            return {
                lux::semantic::typeId(Traits::CanonicalName),
                Traits::CanonicalName,
                Traits::AbiKind,
                Traits::Size,
                Traits::Alignment
            };
        }
    }

    template <class Signature> class TScriptHookEndpoint;

    template <class... Parameters> class TScriptHookEndpoint<void(Parameters...)>
    {
    public:
        TScriptHookEndpoint(
            lux::system::SystemInstanceId system,
            HookPointId id,
            THookPoint<void(Parameters...)>& endpoint
        ) noexcept
            : system_(system), id_(id), endpoint_(&endpoint)
        {
            endpoint_->binding_system_ = system;
            endpoint_->binding_hook_ = id;
        }

        TScriptHookEndpoint(const TScriptHookEndpoint&) = delete;
        TScriptHookEndpoint& operator=(const TScriptHookEndpoint&) = delete;
        TScriptHookEndpoint(TScriptHookEndpoint&&) = delete;
        TScriptHookEndpoint& operator=(TScriptHookEndpoint&&) = delete;

        [[nodiscard]] ScriptHookEndpointDescriptor descriptor() noexcept
        {
            return {
                system_,
                id_,
                lux::simulation::detail::TEndpointSignatureStorage<void(Parameters...)>::view(),
                this,
                &connect,
                &disconnect,
                [](void* context, const void* owner) noexcept {
                    static_cast<TScriptHookEndpoint*>(context)->endpoint_->binding_owner_ = owner;
                },
                [](void* context) noexcept { return static_cast<TScriptHookEndpoint*>(context)->lane_ != nullptr; }
            };
        }

    private:
        static EndpointConnectResult connect(void* context, void* lane_context, ScriptHookLane lane) noexcept
        {
            auto& self = *static_cast<TScriptHookEndpoint*>(context);
            if (lane == nullptr)
                return {{}, EEndpointMutationError::INVALID_CALLBACK};
            if (self.lane_ != nullptr)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            self.lane_context_ = lane_context;
            self.lane_ = lane;
            auto result = self.endpoint_->connect(&self, &dispatch);
            if (!result)
            {
                self.lane_context_ = nullptr;
                self.lane_ = nullptr;
            }
            return result;
        }

        static EEndpointMutationError disconnect(void* context, EndpointConnectionToken token) noexcept
        {
            auto& self = *static_cast<TScriptHookEndpoint*>(context);
            const auto result = self.endpoint_->disconnect(token);
            if (result == EEndpointMutationError::NONE)
            {
                self.lane_context_ = nullptr;
                self.lane_ = nullptr;
            }
            return result;
        }

        static void dispatch(void* context, Parameters... parameters) noexcept
        {
            auto& self = *static_cast<TScriptHookEndpoint*>(context);
            auto slots = detail::argumentSlots(parameters...);
            lux_script_call_frame
                frame{slots.data(), static_cast<std::uint32_t>(slots.size()), 0U, nullptr, 0U, 0U, nullptr};
            self.lane_(self.lane_context_, frame);
        }

        lux::system::SystemInstanceId system_;
        HookPointId id_;
        THookPoint<void(Parameters...)>* endpoint_{};
        void* lane_context_{};
        ScriptHookLane lane_{};
    };

    template <class... Parameters>
    class TScriptHookEndpoint<void(Parameters...) noexcept> final : public TScriptHookEndpoint<void(Parameters...)>
    {
        using Base = TScriptHookEndpoint<void(Parameters...)>;

    public:
        using Base::Base;
    };

    template <class Route, class Payload> class TScriptEventEndpoint final
    {
    public:
        using PayloadCopy = detail::EventPayloadCopy<Payload>;

        TScriptEventEndpoint(
            lux::system::SystemInstanceId system,
            EventPointId id,
            THookChannel<Route, Payload>& channel,
            PayloadCopy copy = detail::defaultEventPayloadCopy<Payload>()
        ) noexcept
            : system_(system), id_(id), channel_(&channel), payload_copy_(copy)
        {}

        TScriptEventEndpoint(const TScriptEventEndpoint&) = delete;
        TScriptEventEndpoint& operator=(const TScriptEventEndpoint&) = delete;

        [[nodiscard]] std::size_t connectionCount() const noexcept
        {
            return lane_ != nullptr ? 1U : 0U;
        }

        [[nodiscard]] ScriptEventEndpointDescriptor descriptor() noexcept
        {
            constexpr auto route = std::is_same_v<Route, SimulationBroadcastRoute> ? EEventRoute::SIMULATION_BROADCAST
                                                                                   : EEventRoute::ENTITY_TARGETED;
            auto copy = payload_copy_ != nullptr ? &copyPayload<false> : nullptr;
            constexpr bool is_default_scalar_layout = detail::defaultEventPayloadCopy<Payload>() != nullptr &&
                                                      lux::semantic::TTypeTraits<Payload>::Size == sizeof(Payload);
            if constexpr (is_default_scalar_layout)
            {
                if (payload_copy_ == detail::defaultEventPayloadCopy<Payload>())
                    copy = &copyPayload<true>;
            }
            ScriptEventPayloadProjection projection{detail::eventPayloadLayout<Payload>(), copy};
            if constexpr (is_default_scalar_layout)
                if (payload_copy_ == detail::defaultEventPayloadCopy<Payload>())
                    projection.non_reentrant_copy_ = copy;
            return {
                system_,
                id_,
                route,
                lux::semantic::makeType<Payload>(lux::semantic::EValuePass::CONST_REF),
                projection,
                this,
                &connect,
                &disconnect,
                &consume,
                [](void* context) noexcept { return self(context).channel_->failed(); },
                channel_,
                [](void* context) noexcept { return self(context).lane_ != nullptr; }
            };
        }

    private:
        static TScriptEventEndpoint& self(void* context) noexcept
        {
            return *static_cast<TScriptEventEndpoint*>(context);
        }

        static EndpointConnectResult connect(void* context, void* lane_context, ScriptEventLane lane) noexcept
        {
            auto& endpoint = self(context);
            const auto busy = endpoint.channel_->mutationError();
            if (busy != EEndpointMutationError::NONE)
                return {{}, busy};
            if (lane == nullptr)
                return {{}, EEndpointMutationError::INVALID_CALLBACK};
            if (endpoint.lane_ != nullptr)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            endpoint.lane_context_ = lane_context;
            endpoint.lane_ = lane;
            return {{0U, endpoint.generation_}, EEndpointMutationError::NONE};
        }

        static EEndpointMutationError disconnect(void* context, EndpointConnectionToken token) noexcept
        {
            auto& endpoint = self(context);
            const auto busy = endpoint.channel_->mutationError();
            if (busy != EEndpointMutationError::NONE)
                return busy;
            if (token.slot != 0U || token.generation != endpoint.generation_ || endpoint.lane_ == nullptr)
                return EEndpointMutationError::INVALID_TOKEN;
            endpoint.lane_ = nullptr;
            endpoint.lane_context_ = nullptr;
            ++endpoint.generation_;
            return EEndpointMutationError::NONE;
        }

        static std::size_t consume(void* context) noexcept
        {
            auto& endpoint = self(context);
            if (endpoint.lane_ == nullptr || endpoint.consuming_ || !endpoint.channel_->scriptConsumptionAllowed())
                return 0U;
            endpoint.consuming_ = true;
            std::size_t calls{};
            for (std::size_t lane{}; lane < endpoint.channel_->laneCount(); ++lane)
            {
                for (const auto& occurrence : endpoint.channel_->lane(lane))
                {
                    auto slot = detail::argumentSlot(occurrence.payload);
                    lux_script_call_frame frame{&slot, 1U, 0U, nullptr, 0U, 0U, nullptr};
                    if constexpr (std::is_same_v<Route, SimulationBroadcastRoute>)
                        endpoint.lane_(endpoint.lane_context_, ecs::NullEntity, frame);
                    else
                        endpoint.lane_(endpoint.lane_context_, occurrence.target, frame);
                    ++calls;
                }
            }
            endpoint.consuming_ = false;
            return calls;
        }

        template <bool DefaultScalar>
        static bool copyPayload(void* context, const lux_script_value_slot& input, std::span<std::byte> output) noexcept
        {
            using Traits = lux::semantic::TTypeTraits<Payload>;
            constexpr auto type = lux::semantic::typeId(Traits::CanonicalName);
            const bool is_invalid_input = input.data == nullptr || input.kind != Traits::AbiKind ||
                                          input.type_id != type || input.size != Traits::Size ||
                                          output.size() != Traits::Size;
            if (is_invalid_input)
                return false;
            if constexpr (DefaultScalar)
            {
                std::memcpy(output.data(), input.data, sizeof(Payload));
                return true;
            }
            else
            {
                // The immutable callback was selected when this descriptor was prepared.
                return self(context).payload_copy_(*static_cast<const Payload*>(input.data), output);
            }
        }

        lux::system::SystemInstanceId system_;
        EventPointId id_;
        THookChannel<Route, Payload>* channel_{};
        bool consuming_{};
        PayloadCopy payload_copy_{};
        void* lane_context_{};
        ScriptEventLane lane_{};
        std::uint32_t generation_{1U};
    };
}
