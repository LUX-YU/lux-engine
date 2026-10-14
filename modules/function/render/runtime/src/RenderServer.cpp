#include "Wire.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace
    {
        struct View final { std::array<float, 4> color; };
        struct Scene final { cxx::SlotMap<View, ViewTag> views; };
        struct Target final { TargetDesc description; std::uint64_t epoch = 1; };
        struct RenderEffect final { std::shared_ptr<const vulkan::CompiledGraph> code; };
        struct Undelivered final { std::shared_ptr<ipc::ReceivedRequest> request; ipc::IpcMessage output; };
        struct Output final { OwnedWireBytes header; };

        TargetDesc targetDescription(const Record& input) noexcept
        {
            return {input.args[8], input.args[9], static_cast<rdesc::ETextureFormat>(input.args[10]),
                input.args[11], static_cast<ETargetKind>(input.args[12])};
        }
    }

    struct RenderServer::State final
    {
        RenderServerConfig config;
        ipc::IpcResponder responder;
        SessionIdentity identity;
        std::vector<FeatureRegistration> features;
        cxx::SlotMap<ERenderCommand, OperationTag> operations;
        std::array<OperationId, operation_count> operation_ids;
        cxx::SlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<Undelivered> undelivered;
        unsigned clients = 0;
        std::atomic<bool> stop{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};

        State(RenderServerConfig value, ipc::IpcResponder input, SessionIdentity session)
            : config(std::move(value)), responder(std::move(input)), identity(session)
        {
            undelivered.reserve(128);
            for (std::size_t i = 0; i < operation_count; ++i)
                operation_ids[i] = operations.emplace(static_cast<ERenderCommand>(i));
        }

        static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*, void* user) noexcept
        {
            auto& state = *static_cast<State*>(user);
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.validation_errors;
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.validation_warnings;
            return VK_FALSE;
        }

        RenderResult<void> install(const std::shared_ptr<Record>& input,
            const std::shared_ptr<ipc::ReceivedRequest>& request, Record& reply) noexcept
        {
            cxx::BinaryReader reader(input->body.view(), config.max_wire_bytes);
            std::uint32_t count = 0;
            if (!reader.readUnsigned(count) || count == 0 || count > max_features)
                return cxx::unexpected(RenderError{kInvalidWire});
            std::vector<WireView> configs;
            std::vector<SelectedFeature> selected;
            std::vector<OwnedWireBytes> config_owners, attachments;
            std::vector<PassProgram> programs;
            configs.reserve(count);
            selected.reserve(count);
            config_owners.reserve(count);
            for (auto view : request->message().attachments())
                attachments.push_back(OwnedWireBytes::fromOwner(request, view));
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::string_view type, name;
                cxx::SchemaVersion version;
                std::span<const std::byte> schema, config_bytes;
                const bool valid = readText(reader, type) && readText(reader, name) &&
                    reader.readUnsigned(version.major) && reader.readUnsigned(version.minor) &&
                    readBlock(reader, schema) && readBlock(reader, config_bytes);
                if (!valid) return cxx::unexpected(RenderError{kInvalidWire});
                const auto registration = std::find_if(features.begin(), features.end(), [&](const auto& f) {
                    return f.definition->type.name() == type;
                });
                if (registration == features.end() || registration->definition->version != version)
                    return cxx::unexpected(RenderError{kFeatureContract});
                const auto& definition = *registration->definition;
                const WireView* wire = nullptr;
                if (definition.config_schema)
                {
                    if (!std::ranges::equal(schemaBytes(*definition.config_schema), schema))
                        return cxx::unexpected(RenderError{kDataSchemaMismatch});
                    config_owners.push_back(OwnedWireBytes::fromOwner(input, config_bytes));
                    configs.push_back({*definition.config_schema, config_owners.back(), attachments});
                    wire = &configs.back();
                }
                else if (!schema.empty() || !config_bytes.empty()) return cxx::unexpected(RenderError{kInvalidWire});
                selected.push_back({&definition, wire, name});
            }
            if (!reader.requireFullyConsumed()) return cxx::unexpected(RenderError{kInvalidWire});
            auto assembly = assembleFeatures(selected, {config.max_wire_bytes, SIZE_MAX});
            if (!assembly) return cxx::unexpected(assembly.error());
            for (const auto& selection : selected)
            {
                const auto registration = std::find_if(features.begin(), features.end(), [&](const auto& f) {
                    return f.definition == selection.definition;
                });
                auto values = registration->programs(selection.config, selection.instance_key);
                if (!values) return cxx::unexpected(values.error());
                for (auto& value : *values) programs.push_back(std::move(value));
            }
            auto compiled = vulkan::compileGraph(*device, *assembly, programs, targetDescription(*input));
            if (!compiled) return cxx::unexpected(compiled.error());
            auto code = std::make_shared<vulkan::CompiledGraph>(std::move(*compiled));
            const auto effect = effects.emplace(RenderEffect{std::move(code)});
            putKey(std::span(reply.args).subspan(6), effect);
            ++compiled_graphs;
            return {};
        }

        RenderResult<void> execute(ERenderCommand operation, const std::shared_ptr<Record>& input,
            const std::shared_ptr<ipc::ReceivedRequest>& request, Record& reply) noexcept
        {
            const auto scene_id = key<SceneId>(std::span(input->args).subspan(0));
            const auto view_id = key<ViewId>(std::span(input->args).subspan(2));
            const auto target_id = key<TargetId>(std::span(input->args).subspan(4));
            const auto effect_id = key<EffectId>(std::span(input->args).subspan(6));
            switch (operation)
            {
            case ERenderCommand::READY:
                if (clients == 2) return cxx::unexpected(RenderError{kRuntimeCapacity});
                reply.body = own(capabilities(operation_ids));
                if (reply.body.size() + record_size > input->reply_capacity)
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                ++clients;
                return {};
            case ERenderCommand::CREATE_SCENE:
                putKey(std::span(reply.args).subspan(0), scenes.emplace());
                return {};
            case ERenderCommand::REMOVE_SCENE:
                if (!scenes.erase(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                return {};
            case ERenderCommand::CREATE_VIEW:
            case ERenderCommand::REMOVE_VIEW:
            {
                auto* scene = scenes.find(scene_id);
                if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (operation == ERenderCommand::REMOVE_VIEW)
                {
                    if (!scene->views.erase(view_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                }
                else
                {
                    if (!std::ranges::all_of(input->color, [](float x) { return std::isfinite(x); }))
                        return cxx::unexpected(RenderError{kInvalidWire});
                    putKey(std::span(reply.args).subspan(2), scene->views.emplace(View{input->color}));
                }
                return {};
            }
            case ERenderCommand::CREATE_TARGET:
            case ERenderCommand::RESIZE_TARGET:
            {
                const auto desc = targetDescription(*input);
                if (auto valid = desc.validate(); !valid) return valid;
                if (desc.kind != ETargetKind::OFFSCREEN) return cxx::unexpected(RenderError{kRuntimeUnsupported});
                if (operation == ERenderCommand::CREATE_TARGET)
                    putKey(std::span(reply.args).subspan(4), targets.emplace(Target{desc}));
                else
                {
                    auto* target = targets.find(target_id);
                    if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
                    target->description = desc;
                    ++target->epoch;
                }
                return {};
            }
            case ERenderCommand::CREATE_EFFECT: return install(input, request, reply);
            case ERenderCommand::REMOVE_EFFECT:
                if (!effects.erase(effect_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                return {};
            case ERenderCommand::STOP: stop.store(true); return {};
            default: return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }

        void dispatch(ipc::ReceivedRequest received) noexcept
        {
            auto request = std::make_shared<ipc::ReceivedRequest>(std::move(received));
            auto bytes = OwnedWireBytes::fromOwner(request, request->message().bytes());
            auto decoded = decodeWire<Record>({DataSchema<Record>::describe(), bytes, {}},
                {config.max_wire_bytes, config.max_wire_bytes});
            if (!decoded)
            {
                // Invalid framing cannot safely identify a business request. End this
                // epoch and release the original opaque input only at cold shutdown.
                undelivered.push_back({std::move(request), {}});
                stop.store(true);
                return;
            }
            auto input = std::make_shared<Record>(std::move(*decoded));
            Record reply;
            reply.request = input->request;
            reply.session = identity;
            reply.args = input->args;
            ERenderCommand operation = ERenderCommand::READY;
            RenderResult<void> result;
            if (!input->operation.isNull())
            {
                const auto* found = operations.find(input->operation);
                if (!found || *found == ERenderCommand::READY || input->session != identity)
                    result = cxx::unexpected(RenderError{kRuntimeIdentity});
                else operation = *found;
            }
            const bool invalid_reply_capacity = input->reply_capacity > config.max_reply_bytes ||
                input->reply_capacity < record_size;
            if (invalid_reply_capacity) result = cxx::unexpected(RenderError{kRuntimeCapacity});
            if (result) result = execute(operation, input, request, reply);
            reply.args[13] = static_cast<unsigned>(result ? EOperationOutcome::SUCCESS : EOperationOutcome::FAILED);
            if (!result) reply.error = result.error();
            auto encoded = encodeWire(reply, {}, wire_limits);
            if (!encoded) std::terminate(); // bounded built-in reply grammar
            if (encoded->size() > input->reply_capacity)
            {
                reply.body = {};
                reply.args[13] = static_cast<unsigned>(EOperationOutcome::FAILED);
                reply.error = {kRuntimeCapacity};
                encoded = encodeWire(reply, {}, wire_limits);
                if (!encoded) std::terminate();
            }
            auto owner = std::make_unique<Output>(Output{std::move(*encoded)});
            auto message = ipc::IpcMessage::takeOwner(owner, owner->header.view());
            if (!message) std::terminate();
            // All WireView/Feature borrows ended before transferring input back.
            input.reset();
            bytes = {};
            if (!responder.tryReply(*request, *message))
            {
                undelivered.push_back({std::move(request), std::move(*message)});
                if (undelivered.size() == undelivered.capacity()) stop.store(true);
            }
            ++commands;
        }

        void shutdown() noexcept
        {
            // There are no background CPU borrowers in this minimal implementation.
            // Host joins run before destruction. Settle every borrowed request first.
            undelivered.clear();
            responder.reset();
            effects.clear();
            device.reset();
            instance.reset();
            running.store(false);
        }
    };

    RenderServer::RenderServer(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (state_->running.load()) std::terminate(); // host must requestStop + join
    }

    RenderResult<std::unique_ptr<RenderServer>> RenderServer::create(RenderServerConfig config) noexcept
    {
        const bool invalid = config.requests_per_tick == 0 || config.requests_per_tick > 128 ||
            config.max_wire_bytes < record_size || config.max_wire_bytes > wire_limits.max_wire_bytes ||
            config.max_reply_bytes < record_size || config.max_reply_bytes > 256 * 1024 * 1024;
        if (invalid) return cxx::unexpected(RenderError{kRuntimeCapacity});
        SessionIdentity identity{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(identity.data()), sizeof(identity),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return cxx::unexpected(RenderError{kRuntimeState});
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderServer>(new RenderServer(
            std::make_unique<State>(std::move(config), std::move(*responder), identity)));
    }

    RenderResult<void> RenderServer::registerFeature(FeatureRegistration feature) noexcept
    {
        auto& s = *state_;
        const bool invalid = s.started.load() || !feature.definition || !feature.programs ||
            (feature.definition && (!feature.definition->describe || feature.definition->type.name().empty()));
        if (invalid) return cxx::unexpected(RenderError{kRuntimeState});
        for (const auto& current : s.features)
            if (current.definition->type.name() == feature.definition->type.name())
                return cxx::unexpected(RenderError{kFeatureDuplicate});
        s.features.push_back(feature);
        return {};
    }

    RenderResult<void> RenderServer::run() noexcept
    {
        auto& s = *state_;
        if (s.started.exchange(true)) return cxx::unexpected(RenderError{kRuntimeState});
        s.running.store(true);
        auto instance = vulkan::InstanceContext::create({{}, s.config.validation, State::diagnostic, &s});
        if (!instance) { s.shutdown(); return cxx::unexpected(instance.error()); }
        s.instance.emplace(std::move(*instance));
        vulkan::DeviceOptions options;
        options.dynamic_rendering = true;
        options.physical_device_index = s.config.physical_device_index;
        auto device = vulkan::DeviceSession::create(*s.instance, options);
        if (!device) { s.shutdown(); return cxx::unexpected(device.error()); }
        s.device = std::move(*device);
        while (!s.stop.load())
        {
            for (std::uint32_t i = 0; i < s.config.requests_per_tick && !s.stop.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED) s.dispatch(std::move(request));
            }
            ++s.ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s.shutdown();
        return {};
    }

    void RenderServer::requestStop() noexcept { state_->stop.store(true); }

    RenderServerStats RenderServer::stats() const noexcept
    {
        const auto& s = *state_;
        return {s.ticks.load(), s.commands.load(), s.compiled_graphs.load(), s.submissions.load(),
            s.validation_errors.load(), s.validation_warnings.load()};
    }
}
