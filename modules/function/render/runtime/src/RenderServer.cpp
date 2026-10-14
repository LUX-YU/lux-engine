#include "Wire.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
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
        struct ExecutionGroup final
        {
            SceneId scene;
            ViewId view;
            TargetId target;
            vulkan::ExecutionContext* context;
        };
        struct RenderEffect final
        {
            std::shared_ptr<const vulkan::CompiledGraph> code;
            std::vector<ExecutionGroup> groups;
            std::size_t next_group = 0;
        };
        struct Undelivered final { std::shared_ptr<ipc::ReceivedRequest> request; ipc::IpcMessage output; };
        struct PendingReadback final
        {
            std::shared_ptr<ipc::ReceivedRequest> request;
            Record reply;
            vulkan::ExecutionContext* context;
        };
        struct Output final
        {
            OwnedWireBytes header;
            OwnedWireBytes data;
            std::array<std::span<const std::byte>, 1> views;
        };

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
        std::vector<BufferInitializer> initializers;
        cxx::SlotMap<ERenderCommand, OperationTag> operations;
        std::array<OperationId, operation_count> operation_ids;
        cxx::SlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<Undelivered> undelivered;
        std::vector<PendingReadback> readbacks;
        unsigned clients = 0;
        std::size_t next_effect = 0;
        std::atomic<bool> stop{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        State(RenderServerConfig value, ipc::IpcResponder input, SessionIdentity session)
            : config(std::move(value)), responder(std::move(input)), identity(session)
        {
            undelivered.reserve(128);
            readbacks.reserve(128);
            for (std::size_t i = 0; i < operation_count; ++i)
                operation_ids[i] = operations.emplace(static_cast<ERenderCommand>(i));
        }

        void finish(std::shared_ptr<ipc::ReceivedRequest> request, Record reply, OwnedWireBytes data = {}) noexcept
        {
            auto encoded = encodeWire(reply, {}, wire_limits);
            if (!encoded) std::terminate();
            const bool exceeds_budget = encoded->size() > reply.reply_capacity ||
                data.size() > reply.reply_capacity - std::min<std::uint64_t>(encoded->size(), reply.reply_capacity);
            if (exceeds_budget)
            {
                reply.body = {};
                data = {};
                reply.args[13] = static_cast<unsigned>(EOperationOutcome::FAILED);
                reply.error = {kRuntimeCapacity};
                encoded = encodeWire(reply, {}, wire_limits);
                if (!encoded) std::terminate();
            }
            auto owner = std::make_unique<Output>(Output{std::move(*encoded), std::move(data)});
            owner->views[0] = owner->data.view();
            const auto views = owner->data.empty() ? std::span<const std::span<const std::byte>>{} :
                std::span<const std::span<const std::byte>>(owner->views);
            auto message = ipc::IpcMessage::takeOwner(owner, owner->header.view(), views);
            if (!message) std::terminate();
            if (!responder.tryReply(*request, *message))
            {
                undelivered.push_back({std::move(request), std::move(*message)});
                if (undelivered.size() >= 128) stop.store(true);
            }
        }

        void cancelReadbacks(vulkan::ExecutionContext* context) noexcept
        {
            for (auto it = readbacks.begin(); it != readbacks.end();)
            {
                if (it->context != context) { ++it; continue; }
                it->context->releaseReadback();
                it->reply.args[13] = static_cast<unsigned>(EOperationOutcome::CANCELLED);
                finish(std::move(it->request), std::move(it->reply));
                it = readbacks.erase(it);
            }
        }

        RenderResult<void> withdraw(RenderEffect& effect, std::size_t index) noexcept
        {
            auto* context = effect.groups[index].context;
            cancelReadbacks(context);
            if (auto result = device->release(context); !result) return result;
            effect.groups.erase(effect.groups.begin() + index);
            return {};
        }

        template <class Predicate>
        RenderResult<void> admitWithdraw(Predicate matches) const noexcept
        {
            std::size_t needed = 0;
            for (const auto& effect : effects)
                for (const auto& group : effect.groups)
                    if (matches(effect, group))
                        if (auto ticket = group.context->lastUse(); ticket &&
                            ticket->serial() > device->queue().completed()) ++needed;
            const auto& retired = device->retirement();
            if (needed > retired.capacity() - retired.pending())
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            return {};
        }

        RenderResult<void> attach(const Record& input) noexcept
        {
            const auto scene_id = key<SceneId>(std::span(input.args).subspan(0));
            const auto view_id = key<ViewId>(std::span(input.args).subspan(2));
            const auto target_id = key<TargetId>(std::span(input.args).subspan(4));
            const auto effect_id = key<EffectId>(std::span(input.args).subspan(6));
            const auto* scene = scenes.find(scene_id);
            const auto* view = scene ? scene->views.find(view_id) : nullptr;
            const auto* target = targets.find(target_id);
            auto* effect = effects.find(effect_id);
            const bool invalid = !view || !target || !effect;
            if (invalid) return cxx::unexpected(RenderError{kRuntimeIdentity});
            for (const auto& existing : effects)
                for (const auto& group : existing.groups)
                    if (group.target == target_id) return cxx::unexpected(RenderError{kRuntimeState});
            auto prepared = device->prepare(effect->code, target->description, {scene_id, view_id}, initializers);
            if (!prepared) return cxx::unexpected(prepared.error());
            (*prepared)->setColor(view->color);
            effect->groups.push_back({scene_id, view_id, target_id, *prepared});
            scoped_buffers_created.store(device->scopedBufferCreations());
            return {};
        }

        RenderResult<void> resize(TargetId id, Target& target, const TargetDesc& description) noexcept
        {
            ExecutionGroup* group = nullptr;
            std::shared_ptr<const vulkan::CompiledGraph> code;
            for (auto& effect : effects)
                for (auto& candidate : effect.groups)
                    if (candidate.target == id) { group = &candidate; code = effect.code; }
            if (group)
            {
                if (auto admitted = admitWithdraw([&](const auto&, const auto& current) {
                    return current.target == id;
                }); !admitted) return admitted;
                // One Target writer, proven on attach. Resize builds one candidate without recompilation.
                auto candidate = device->prepare(std::move(code), description,
                    {group->scene, group->view}, initializers);
                if (!candidate) return cxx::unexpected(candidate.error());
                (*candidate)->setColor(group->context->color());
                cancelReadbacks(group->context);
                if (auto result = device->release(group->context); !result)
                {
                    if (!device->release(*candidate)) std::terminate(); // never submitted
                    return result;
                }
                group->context = *candidate;
            }
            target.description = description;
            ++target.epoch;
            scoped_buffers_created.store(device->scopedBufferCreations());
            return {};
        }

        RenderResult<void> queueReadback(const Record& input,
            const std::shared_ptr<ipc::ReceivedRequest>& request, Record reply) noexcept
        {
            auto* effect = effects.find(key<EffectId>(std::span(input.args).subspan(6)));
            if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
            const auto scene = key<SceneId>(std::span(input.args).subspan(0));
            const auto view = key<ViewId>(std::span(input.args).subspan(2));
            const auto target = key<TargetId>(std::span(input.args).subspan(4));
            auto found = std::find_if(effect->groups.begin(), effect->groups.end(), [&](const auto& group) {
                return group.scene == scene && group.view == view && group.target == target;
            });
            if (found == effect->groups.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
            const bool full = readbacks.size() == readbacks.capacity() ||
                found->context->readbackBytes() > input.reply_capacity - record_size;
            if (full) return cxx::unexpected(RenderError{kRuntimeCapacity});
            found->context->retainReadback();
            readbacks.push_back({request, std::move(reply), found->context});
            return {};
        }

        RenderResult<void> tick() noexcept
        {
            for (std::size_t e = 0; e < effects.size(); ++e)
            {
                auto& effect = *(effects.begin() + (next_effect + e) % effects.size());
                for (std::size_t g = 0; g < effect.groups.size(); ++g)
                {
                    auto& group = effect.groups[(effect.next_group + g) % effect.groups.size()];
                    auto result = group.context->execute();
                    if (!result) return cxx::unexpected(result.error());
                    if (*result) ++submissions;
                }
                effect.next_group = effect.groups.empty() ? 0 : (effect.next_group + 1) % effect.groups.size();
            }
            next_effect = effects.empty() ? 0 : (next_effect + 1) % effects.size();
            for (auto it = readbacks.begin(); it != readbacks.end();)
            {
                auto result = it->context->readback();
                if (!result && result.error().type == vulkan::kBusy) { ++it; continue; }
                it->context->releaseReadback();
                it->reply.args[13] = static_cast<unsigned>(result ? EOperationOutcome::SUCCESS : EOperationOutcome::FAILED);
                if (!result) it->reply.error = result.error();
                finish(std::move(it->request), std::move(it->reply), result ? own(std::move(*result)) : OwnedWireBytes{});
                it = readbacks.erase(it);
            }
            return device->collect();
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
            std::vector<const FeatureRegistration*> registrations;
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
                registrations.push_back(&*registration);
            }
            if (!reader.requireFullyConsumed()) return cxx::unexpected(RenderError{kInvalidWire});
            auto assembly = assembleFeatures(selected, {config.max_wire_bytes, SIZE_MAX});
            if (!assembly) return cxx::unexpected(assembly.error());
            for (const auto& contribution : assembly->contributions)
            {
                const auto index = contribution.selection_index;
                const auto& selection = selected[index];
                auto values = registrations[index]->programs(selection.config, selection.instance_key);
                if (!values) return cxx::unexpected(values.error());
                const auto passes = assembly->definition.passes().subspan(contribution.first_pass,
                    contribution.pass_count);
                for (std::size_t p = 0; p < values->size(); ++p)
                {
                    const auto& value = (*values)[p];
                    if (std::ranges::none_of(passes, [&](const auto& pass) { return pass.key == value.pass_key; }))
                        return cxx::unexpected(RenderError{kFeatureOwnership, {index, p}});
                    for (std::size_t previous = 0; previous < p; ++previous)
                        if ((*values)[previous].pass_key == value.pass_key)
                            return cxx::unexpected(RenderError{kFeatureDuplicate, {index, p, previous}});
                }
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
                if (!scenes.find(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                    return group.scene == scene_id;
                }); !admitted) return admitted;
                for (auto& effect : effects)
                    for (std::size_t i = effect.groups.size(); i-- > 0;)
                        if (effect.groups[i].scene == scene_id)
                            if (auto result = withdraw(effect, i); !result) return result;
                if (!scenes.erase(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                return {};
            case ERenderCommand::CREATE_VIEW:
            case ERenderCommand::REMOVE_VIEW:
            {
                auto* scene = scenes.find(scene_id);
                if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (operation == ERenderCommand::REMOVE_VIEW)
                {
                    if (!scene->views.find(view_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                    if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                        return group.scene == scene_id && group.view == view_id;
                    }); !admitted) return admitted;
                    for (auto& effect : effects)
                        for (std::size_t i = effect.groups.size(); i-- > 0;)
                            if (effect.groups[i].scene == scene_id && effect.groups[i].view == view_id)
                                if (auto result = withdraw(effect, i); !result) return result;
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
                {
                    putKey(std::span(reply.args).subspan(4), targets.emplace(Target{desc}));
                    reply.args[14] = 1;
                    reply.args[15] = 0;
                }
                else
                {
                    auto* target = targets.find(target_id);
                    if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
                    if (target->epoch == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
                    if (auto changed = resize(target_id, *target, desc); !changed) return changed;
                    reply.args[14] = static_cast<std::uint32_t>(target->epoch);
                    reply.args[15] = static_cast<std::uint32_t>(target->epoch >> 32);
                }
                return {};
            }
            case ERenderCommand::CREATE_EFFECT: return install(input, request, reply);
            case ERenderCommand::REMOVE_EFFECT:
            {
                auto* effect = effects.find(effect_id);
                if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (auto admitted = admitWithdraw([&](const auto& owner, const auto&) {
                    return &owner == effect;
                }); !admitted) return admitted;
                while (!effect->groups.empty())
                    if (auto result = withdraw(*effect, effect->groups.size() - 1); !result) return result;
                if (!effects.erase(effect_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
                return {};
            }
            case ERenderCommand::ATTACH: return attach(*input);
            case ERenderCommand::DETACH:
            {
                auto* effect = effects.find(effect_id);
                if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
                for (std::size_t i = 0; i < effect->groups.size(); ++i)
                {
                    const auto& group = effect->groups[i];
                    if (group.scene == scene_id && group.view == view_id && group.target == target_id)
                    {
                        if (auto admitted = admitWithdraw([&](const auto&, const auto& current) {
                            return current.context == group.context;
                        }); !admitted) return admitted;
                        return withdraw(*effect, i);
                    }
                }
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
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
            reply.reply_capacity = input->reply_capacity;
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
            if (result && operation == ERenderCommand::READBACK)
            {
                result = queueReadback(*input, request, reply);
                if (result) { ++commands; return; }
            }
            else
            if (result) result = execute(operation, input, request, reply);
            reply.args[13] = static_cast<unsigned>(result ? EOperationOutcome::SUCCESS : EOperationOutcome::FAILED);
            if (!result) reply.error = result.error();
            // All WireView/Feature borrows ended before transferring input back.
            input.reset();
            bytes = {};
            finish(std::move(request), std::move(reply));
            ++commands;
        }

        void shutdown() noexcept
        {
            // There are no background CPU borrowers in this minimal implementation.
            // Host joins run before destruction. Settle every borrowed request first.
            while (!readbacks.empty()) cancelReadbacks(readbacks.back().context);
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
            config.max_reply_bytes < record_size || config.max_reply_bytes > 256 * 1024 * 1024 ||
            config.retirement_capacity == 0;
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
        for (std::size_t i = 0; i < feature.buffers.size(); ++i)
        {
            const auto& buffer = feature.buffers[i];
            if (buffer.type.name().empty() || !buffer.initialize) return cxx::unexpected(RenderError{kFeatureContract});
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& earlier = feature.buffers[j];
                const bool conflict = earlier.type.name() == buffer.type.name() && earlier.abi == buffer.abi &&
                    earlier.initialize != buffer.initialize;
                if (conflict) return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
            for (const auto& current : s.initializers)
                if (current.type.name() == buffer.type.name() && current.abi == buffer.abi &&
                    current.initialize != buffer.initialize) return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        for (const auto& buffer : feature.buffers)
            if (std::ranges::none_of(s.initializers, [&](const auto& current) {
                return current.type.name() == buffer.type.name() && current.abi == buffer.abi;
            })) s.initializers.push_back(buffer);
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
        auto device = vulkan::DeviceSession::create(*s.instance, options, 3, s.config.retirement_capacity);
        if (!device) { s.shutdown(); return cxx::unexpected(device.error()); }
        s.device = std::move(*device);
        RenderResult<void> result;
        while (!s.stop.load())
        {
            for (std::uint32_t i = 0; i < s.config.requests_per_tick && !s.stop.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED) s.dispatch(std::move(request));
            }
            if (s.stop.load()) break;
            if (auto rendered = s.tick(); !rendered) { result = cxx::unexpected(rendered.error()); break; }
            ++s.ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s.shutdown();
        return result;
    }

    void RenderServer::requestStop() noexcept { state_->stop.store(true); }

    RenderServerStats RenderServer::stats() const noexcept
    {
        const auto& s = *state_;
        return {s.ticks.load(), s.commands.load(), s.compiled_graphs.load(), s.submissions.load(),
            s.validation_errors.load(), s.validation_warnings.load(), s.scoped_buffers_created.load()};
    }
}
