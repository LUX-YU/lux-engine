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
    using ScriptHookLane = void (*)(void *, lux_script_call_frame &) noexcept;
    using ScriptEventLane = void (*)(void *, ecs::Entity, lux_script_call_frame &) noexcept;

    struct ScriptEventPayloadProjection final
    {
        lux::semantic::Layout owned_layout;
        bool (*copy)(void*, const lux_script_value_slot&, std::span<std::byte>) noexcept{};
    };

    struct ScriptHookEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        HookPointId hook;
        lux::semantic::SignatureView signature;
        void *context{};
        EndpointConnectResult (*connect)(void *, void *, ScriptHookLane) noexcept {};
        EEndpointMutationError (*disconnect)(void *, EndpointConnectionToken) noexcept {};
        void (*bind_owner)(void*, const void*) noexcept{};
        bool (*connected)(void*) noexcept{};
    };

    struct ScriptEventEndpointDescriptor final
    {
        lux::system::SystemInstanceId system;
        EventPointId event;
        EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
        lux::semantic::Type payload_type;
        ScriptEventPayloadProjection payload_projection;
        void *context{};
        EndpointConnectResult (*connect)(void *, void *, ScriptEventLane) noexcept {};
        EEndpointMutationError (*disconnect)(void *, EndpointConnectionToken) noexcept {};
        bool (*seal)(void*) noexcept{};
        std::size_t (*consume)(void*) noexcept{};
        bool (*failed)(void*) noexcept{};
        void (*reset)(void*) noexcept{};
        void (*discard)(void*) noexcept{};
        void* channel_context{};
        bool (*connected)(void*) noexcept{};
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

        template <class Payload>
        using EventPayloadCopy = bool (*)(const Payload&, std::span<std::byte>) noexcept;

        template <class Payload>
        [[nodiscard]] bool copyScalarEventPayload(
            const Payload& payload,
            std::span<std::byte> output
        ) noexcept
        {
            if (output.size() != sizeof(Payload))
                return false;
            std::memcpy(output.data(), std::addressof(payload), sizeof(Payload));
            return true;
        }

        template <class Payload>
        [[nodiscard]] constexpr EventPayloadCopy<Payload> defaultEventPayloadCopy() noexcept
        {
            constexpr auto kind = lux::semantic::TypeTraits<std::remove_cv_t<Payload>>::AbiKind;
            constexpr bool is_scalar = kind >= static_cast<std::uint8_t>(lux::semantic::EAbiKind::BOOL) &&
                kind <= static_cast<std::uint8_t>(lux::semantic::EAbiKind::F64);
            if constexpr (is_scalar && std::is_trivially_copyable_v<Payload>)
                return &copyScalarEventPayload<Payload>;
            return nullptr;
        }

        template <class Payload>
        [[nodiscard]] constexpr lux::semantic::Layout eventPayloadLayout() noexcept
        {
            using Traits = lux::semantic::TypeTraits<std::remove_cv_t<Payload>>;
            return {
                lux::semantic::typeId(Traits::CanonicalName),
                Traits::CanonicalName,
                Traits::AbiKind,
                Traits::Size,
                Traits::Alignment
            };
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
            endpoint_->binding_system_ = system;
            endpoint_->binding_hook_ = id;
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
                &disconnect,
                [](void* context, const void* owner) noexcept {
                    static_cast<ScriptHookEndpoint*>(context)->endpoint_->binding_owner_ = owner;
                },
                [](void* context) noexcept { return static_cast<ScriptHookEndpoint*>(context)->lane_ != nullptr; }
            };
        }

    private:
        static EndpointConnectResult connect(
            void *context,
            void *lane_context,
            ScriptHookLane lane) noexcept
        {
            auto &self = *static_cast<ScriptHookEndpoint *>(context);
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

        static EEndpointMutationError disconnect(
            void *context,
            EndpointConnectionToken token) noexcept
        {
            auto& self = *static_cast<ScriptHookEndpoint*>(context);
            const auto result = self.endpoint_->disconnect(token);
            if (result == EEndpointMutationError::NONE)
            {
                self.lane_context_ = nullptr;
                self.lane_ = nullptr;
            }
            return result;
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
    class ScriptEventEndpoint final
    {
    public:
        using PayloadCopy = detail::EventPayloadCopy<Payload>;

        ScriptEventEndpoint(
            lux::system::SystemInstanceId system,
            EventPointId id,
            HookChannel<Route, Payload>& channel,
            PayloadCopy copy = detail::defaultEventPayloadCopy<Payload>()
        ) noexcept : system_(system), id_(id), channel_(&channel), payload_copy_(copy)
        {}

        ScriptEventEndpoint(const ScriptEventEndpoint&) = delete;
        ScriptEventEndpoint& operator=(const ScriptEventEndpoint&) = delete;

        [[nodiscard]] std::size_t connectionCount() const noexcept { return lane_ != nullptr ? 1U : 0U; }

        [[nodiscard]] ScriptEventEndpointDescriptor descriptor() noexcept
        {
            constexpr auto route = std::is_same_v<Route, SimulationBroadcastRoute>
                ? EEventRoute::SIMULATION_BROADCAST : EEventRoute::ENTITY_TARGETED;
            return {system_, id_, route,
                lux::semantic::makeType<Payload>(lux::semantic::EValuePass::CONST_REF),
                {detail::eventPayloadLayout<Payload>(), payload_copy_ != nullptr ? &copyPayload : nullptr},
                this, &connect, &disconnect,
                [](void* context) noexcept { return self(context).channel_->seal(); },
                &consume,
                [](void* context) noexcept { return self(context).channel_->failed(); },
                [](void* context) noexcept { self(context).channel_->reset(); },
                [](void* context) noexcept { self(context).channel_->discard(); }, channel_,
                [](void* context) noexcept { return self(context).lane_ != nullptr; }};
        }

    private:
        static ScriptEventEndpoint& self(void* context) noexcept
        {
            return *static_cast<ScriptEventEndpoint*>(context);
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
                    lux_script_call_frame frame{&slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
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

        static bool copyPayload(void* context, const lux_script_value_slot& input, std::span<std::byte> output) noexcept
        {
            const auto& endpoint = self(context);
            using Traits = lux::semantic::TypeTraits<Payload>;
            const bool invalid = input.data == nullptr || input.kind != Traits::AbiKind ||
                input.type_id != lux::semantic::typeId(Traits::CanonicalName) || input.size != Traits::Size ||
                output.size() != Traits::Size || endpoint.payload_copy_ == nullptr;
            return !invalid && endpoint.payload_copy_(*static_cast<const Payload*>(input.data), output);
        }

        lux::system::SystemInstanceId system_;
        EventPointId id_;
        HookChannel<Route, Payload>* channel_{};
        bool consuming_{};
        PayloadCopy payload_copy_{};
        void* lane_context_{};
        ScriptEventLane lane_{};
        std::uint32_t generation_{1U};
    };
}
