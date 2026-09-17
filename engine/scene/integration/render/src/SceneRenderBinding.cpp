#include <algorithm>
#include <cassert>
#include <cmath>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>
#include <lux/engine/scene/detail/RenderSyncStorage.hpp>

namespace lux::scene
{
    namespace
    {
        struct ProgramDrain final
        {
            std::shared_ptr<std::atomic_bool> retired;
            explicit ProgramDrain(std::shared_ptr<std::atomic_bool> value) : retired(std::move(value))
            {
            }
            ~ProgramDrain()
            {
                retired->store(true, std::memory_order_release);
            }
        };

        struct BoundFeature final
        {
            const RenderFeatureMeta *meta{};
            render::FeatureHandle handle{};
        };

        SceneRenderBindingFailure reject(SceneSystemView description, std::uint64_t subject = 0)
        {
            return {{ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), {}, subject}, {}};
        }
    } // namespace

    struct SceneRenderInput::Data final
    {
        std::shared_ptr<const SceneMetaManager> metadata;
        render::FeatureCatalog catalog;
        std::vector<BoundFeature> features;
        std::vector<std::byte> configuration;
        system::SystemInstanceId system{};
        render::RenderSceneId scene{};
        double page_size{};
        std::shared_ptr<detail::RenderSyncStorage> storage;

        ~Data()
        {
            if (storage)
            {
                storage->producer_closed.store(true, std::memory_order_release);
                storage->notify();
            }
        }
    };

    SceneRenderInput::SceneRenderInput(std::unique_ptr<Data> data) noexcept : data_(std::move(data))
    {
    }
    SceneRenderInput::~SceneRenderInput() = default;
    SceneRenderInput::SceneRenderInput(SceneRenderInput &&) noexcept = default;
    SceneRenderInput &SceneRenderInput::operator=(SceneRenderInput &&) noexcept = default;
    render::RenderSceneId SceneRenderInput::sceneId() const noexcept
    {
        return data_->scene;
    }
    double SceneRenderInput::coordinatePageSize() const noexcept
    {
        return data_->page_size;
    }

    lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, SceneSystemBuildFailure> SceneRenderInput::makePipeline(
        simulation::ecs::Registry &registry, SceneSystemView description)
    {
        if (!data_->storage || description.instanceId() != data_->system ||
            !std::ranges::equal(description.configurationPayload(), data_->configuration))
        {
            return lux::cxx::unexpected(reject(description).scene);
        }
        RenderSyncPipeline::StageList stages;
        stages.reserve(data_->features.size());
        for (const auto &feature : data_->features)
        {
            if (!feature.meta->create_sync_stage)
            {
                continue;
            }
            auto stage = feature.meta->create_sync_stage(
                {registry, data_->scene, data_->catalog, feature.meta->type, feature.handle, data_->page_size, {}});
            if (!stage)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::CONSTRUCTION_FAILURE, data_->system, {}, feature.meta->type});
            }
            stages.push_back(std::move(*stage));
        }
        auto result = RenderSyncPipeline::create(std::move(stages), data_->storage, data_->metadata);
        if (!result)
        {
            return lux::cxx::unexpected(
                SceneSystemBuildFailure{ESceneSystemBuildError::CONSTRUCTION_FAILURE, data_->system});
        }
        // Input no longer owns the producer terminal signal. A failed preparation
        // above retains that ownership and remains retryable with the same input.
        data_->storage.reset();
        return std::move(*result);
    }

    struct SceneRenderBinding::Data final
    {
        struct Attachment final
        {
            const RenderFeatureMeta *meta{};
            std::uint32_t type{};
            std::vector<std::byte> wire;
        };
        explicit Data(RenderRuntimeLease lease) : runtime(std::move(lease)), consumer(storage)
        {
        }
        RenderRuntimeLease runtime;
        render::RenderSceneLease scene;
        std::shared_ptr<detail::RenderSyncStorage> storage = std::make_shared<detail::RenderSyncStorage>();
        RenderSyncConsumer consumer;
        std::unique_ptr<SceneRenderInput::Data> input = std::make_unique<SceneRenderInput::Data>();
        std::vector<Attachment> attachments;
        render::RenderRequest<render::SceneCreatedReply> create;
        render::RenderRequest<render::FeatureAddedReply> attach;
        std::size_t next{};
        ESceneRenderBindingState state{ESceneRenderBindingState::CREATING};
        SceneRenderBindingFailure failure{};
        system::SystemInstanceId system{};
        bool input_taken{};
        bool closing{};
        render::RenderProgram<> drain_program;
        std::shared_ptr<std::atomic_bool> drain_retired;
        bool drain_submitted{};
        std::size_t drain_recycles{};
        bool terminal_observed{}, failure_recorded{};

        void fail(render::RenderError error, std::uint64_t subject = 0)
        {
            if (!failure_recorded)
            {
                failure = {{ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE, system, {}, subject}, error};
                failure_recorded = true;
            }
            state = ESceneRenderBindingState::FAILED;
        }
    };

    SceneRenderBinding::SceneRenderBinding(std::unique_ptr<Data> data) noexcept : data_(std::move(data))
    {
    }
    SceneRenderBinding::~SceneRenderBinding()
    {
        // Submitted creation/attachment must be observed before this owner dies.
        assert(data_->state == ESceneRenderBindingState::CLOSED);
    }

    lux::cxx::expected<std::unique_ptr<SceneRenderBinding>, SceneRenderBindingFailure> SceneRenderBinding::begin(
        RenderRuntime &runtime, SceneSystemView description, std::shared_ptr<const SceneMetaManager> metadata)
    {
        const auto registration = builtinRenderSystemRegistration();
        if (!metadata || description.type() != registration.type ||
            description.version() != registration.description->version ||
            description.configurationSchemaName() != registration.description->configuration_schema_name ||
            description.configurationSchemaHash() !=
                lux::cxx::Fnv1a64::hash(registration.description->configuration_schema_name) ||
            description.configurationSchemaVersion() != registration.description->configuration_schema_version)
        {
            return lux::cxx::unexpected(reject(description));
        }
        RenderSystemConfiguration config;
        const auto decoded = registration.configuration.decode(description.configurationPayload(), &config);
        if (!decoded)
        {
            return lux::cxx::unexpected(SceneRenderBindingFailure{{ESceneSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                                                                   description.instanceId(),
                                                                   {},
                                                                   0,
                                                                   decoded.error()},
                                                                  {}});
        }
        if (!std::isfinite(config.coordinate_page_size) || config.coordinate_page_size <= 0)
        {
            return lux::cxx::unexpected(reject(description));
        }
        auto lease = runtime.acquire();
        if (!lease)
        {
            return lux::cxx::unexpected(SceneRenderBindingFailure{
                {ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE, description.instanceId()},
                lease.error().render_error});
        }
        auto data = std::make_unique<Data>(std::move(*lease));
        data->input->metadata = std::move(metadata);
        data->input->catalog = data->runtime.features();
        data->input->configuration.assign(description.configurationPayload().begin(),
                                          description.configurationPayload().end());
        data->input->system = description.instanceId();
        data->system = description.instanceId();
        data->input->page_size = config.coordinate_page_size;
        std::vector<std::string_view> roots;
        for (std::size_t i{}; i < config.features.size(); ++i)
        {
            const auto type = config.features[i].type;
            const auto name = data->input->catalog.nameOfType(type);
            if (name.empty() || std::any_of(config.features.begin(), config.features.begin() + i,
                                            [type](const auto &prior) { return prior.type == type; }))
            {
                return lux::cxx::unexpected(reject(description, type));
            }
            roots.push_back(name);
        }
        const auto order = data->input->catalog.resolveAttachOrder(roots);
        if (!order.unknown.empty() || !order.missing_deps.empty() || !order.cycle.empty())
        {
            return lux::cxx::unexpected(reject(description));
        }
        for (const auto name : order.order)
        {
            const auto *descriptor = data->input->catalog.descriptor(name);
            const auto *meta = descriptor ? data->input->metadata->getRenderFeatureMeta(descriptor->type) : nullptr;
            if (!meta || !meta->scene_configurable || !meta->registration || !meta->registration->configuration.valid())
            {
                return lux::cxx::unexpected(reject(description, descriptor ? descriptor->type : 0));
            }
            const auto selected =
                std::ranges::find(config.features, meta->type, &RenderFeatureInstanceDescription::type);
            const std::span<const std::byte> portable = selected == config.features.end()
                                                            ? meta->default_configuration
                                                            : std::span<const std::byte>(selected->configuration);
            Data::Attachment attachment{meta, data->input->catalog.typeId(name), {}};
            auto prepared = meta->registration->configuration.materialize_attach(portable, attachment.wire);
            if (!prepared || attachment.type == 0 ||
                attachment.wire.size() != meta->registration->configuration.attach_wire_size)
            {
                return lux::cxx::unexpected(reject(description, meta->type));
            }
            data->attachments.push_back(std::move(attachment));
        }
        // All CPU validation precedes the first external effect.
        render::RenderControlSession::CreateSceneConfig create{};
        const std::string name(description.instanceName());
        create.name = name.c_str();
        create.coordinate_page_size = config.coordinate_page_size;
        data->create = data->runtime.control().createScene(create);
        return std::unique_ptr<SceneRenderBinding>(new SceneRenderBinding(std::move(data)));
    }

    std::size_t SceneRenderBinding::poll(std::size_t packet_budget)
    {
        auto &d = *data_;
        std::size_t forwarded{};
        if (d.state == ESceneRenderBindingState::CLOSED)
        {
            return 0;
        }
        const auto runtime = d.runtime.status();
        if (runtime.state != ERenderRuntimeState::ACTIVE)
        {
            d.consumer.stop(); // Wake a blocked producer, even when packet_budget
                               // is zero.
            if (!d.terminal_observed)
            {
                d.fail(runtime.error.ok() ? render::renderError<render::err::comm::ChannelStopping>() : runtime.error);
                d.terminal_observed = true;
            }
            if (!d.closing)
            {
                return 0;
            }
            d.state = ESceneRenderBindingState::CLOSING;
            if (runtime.state != ERenderRuntimeState::RETIRED || (d.input_taken && !d.consumer.producerClosed()))
            {
                return 0;
            }
            // Backend destruction and Main CPU retirement are proven by the
            // runtime. No further Program/Control packet can be accepted. Retire
            // local packets separately; do not turn this into a fictitious
            // forward/drain completion.
            if (d.input_taken)
            {
                d.consumer.retireAfterBackendStopped();
            }
            d.drain_program.clear_keep_capacity();
            d.create = {};
            d.attach = {};
            d.input.reset();
            d.scene.retireAfterBackendStopped();
            d.runtime = {};
            d.state = ESceneRenderBindingState::CLOSED;
            return 0;
        }
        if (d.state == ESceneRenderBindingState::CREATING)
        {
            if (!d.create.isReady())
            {
                return forwarded;
            }
            auto result = d.create.tryResult();
            if (!result)
            {
                d.fail(result.error());
            }
            else if (!result->get().error.ok() || !result->get().scene_id.isValid())
            {
                d.fail(result->get().error);
            }
            else
            {
                d.scene = d.runtime.control().adoptScene(result->get().scene_id);
                d.input->scene = d.scene.id();
                d.state = ESceneRenderBindingState::ATTACHING;
            }
        }
        if (d.state == ESceneRenderBindingState::ATTACHING)
        {
            if (d.attach.valid())
            {
                if (!d.attach.isReady())
                {
                    return forwarded;
                }
                auto result = d.attach.tryResult();
                if (!result)
                {
                    d.fail(result.error(), d.attachments[d.next].meta->type);
                }
                else if (!result->get().error.ok() || !result->get().feature.isValid())
                {
                    d.fail(result->get().error, d.attachments[d.next].meta->type);
                }
                else
                {
                    d.input->features.push_back({d.attachments[d.next].meta, result->get().feature});
                    ++d.next;
                }
                d.attach = {};
            }
            if (d.state == ESceneRenderBindingState::ATTACHING)
            {
                if (d.closing || d.next == d.attachments.size())
                {
                    d.state = ESceneRenderBindingState::READY;
                }
                else
                {
                    const auto &next = d.attachments[d.next];
                    d.attach = d.runtime.control().addFeatureRaw(d.scene.id(), next.type, next.wire);
                }
            }
        }
        if (d.input_taken)
        {
            for (; forwarded < packet_budget; ++forwarded)
            {
                if (d.consumer.tryForwardUpdate(d.runtime.programs()) != ERenderForwardResult::FORWARDED)
                {
                    break;
                }
            }
        }
        if (d.closing && d.state != ESceneRenderBindingState::CREATING &&
            d.state != ESceneRenderBindingState::ATTACHING && d.state != ESceneRenderBindingState::CLOSED)
        {
            d.state = ESceneRenderBindingState::CLOSING;
            if (d.input_taken && (!d.consumer.producerClosed() || d.consumer.hasPendingUpdate() ||
                                  d.runtime.programs().hasPendingSubmit()))
            {
                return forwarded;
            }
            if (d.input_taken && d.storage->published.load(std::memory_order_relaxed) != 0)
            {
                // Client acceptance is not server adoption. The existing owned attachment
                // retires after the ordered Program prefix, before Scene release or resource unpinning.
                if (!d.drain_retired)
                {
                    d.drain_retired = std::make_shared<std::atomic_bool>(false);
                    render::RenderProgramSession::Builder builder(d.drain_program);
                    static_cast<void>(builder.emplaceAttachment<ProgramDrain>(render::attachment_types::OwnedObject,
                                                                              d.drain_retired));
                }
                if (!d.drain_submitted)
                {
                    if (!d.runtime.programs().trySubmitPrepared(d.drain_program))
                    {
                        return forwarded;
                    }
                    d.drain_submitted = true;
                }
                if (!d.drain_retired->load(std::memory_order_acquire))
                {
                    // Persistent Program slots retire attachments when the Main
                    // producer reuses them. Closing the last View must not depend
                    // on another draw. At most one empty StateUpdate per poll,
                    // bounded by one ring rotation; these contain no business
                    // update and issue no GPU Frame submission.
                    if (d.drain_recycles < render::RenderProgramChannel<>::request_slot_count &&
                        d.runtime.programs().trySubmitPrepared(d.drain_program))
                    {
                        ++d.drain_recycles;
                    }
                    return forwarded;
                }
            }
            const auto released = d.scene.close();
            if (released == render::ERenderLeaseCloseStatus::Released ||
                released == render::ERenderLeaseCloseStatus::AlreadyClosed)
            {
                d.state = ESceneRenderBindingState::CLOSED;
                d.runtime = {};
            }
        }
        return forwarded;
    }

    RenderSyncStatistics SceneRenderBinding::statistics() const noexcept
    {
        const auto &s = *data_->storage;
        const auto published = s.published.load(std::memory_order_relaxed);
        return {published,
                s.forwarded.load(std::memory_order_relaxed),
                s.backpressured.load(std::memory_order_relaxed),
                static_cast<std::uint32_t>(s.updates.pendingFrames()),
                published ? 1U : 0U,
                s.retired_unforwarded};
    }

    ESceneRenderBindingState SceneRenderBinding::state() const noexcept
    {
        return data_->state;
    }
    bool SceneRenderBinding::drainSubmitted() const noexcept
    {
        return data_->drain_submitted;
    }
    bool SceneRenderBinding::hasFailure() const noexcept
    {
        return data_->failure_recorded;
    }
    const SceneRenderBindingFailure &SceneRenderBinding::failure() const noexcept
    {
        return data_->failure;
    }
    bool SceneRenderBinding::hasPendingUpdate() const noexcept
    {
        return data_->consumer.hasPendingUpdate();
    }
    void SceneRenderBinding::requestClose() noexcept
    {
        data_->closing = true;
    }

    lux::cxx::expected<SceneRenderInput, SceneRenderBindingFailure> SceneRenderBinding::takeInput()
    {
        auto &d = *data_;
        if (d.state != ESceneRenderBindingState::READY || d.closing || d.input_taken)
        {
            return lux::cxx::unexpected(SceneRenderBindingFailure{{ESceneSystemBuildError::INVALID_DESCRIPTION}, {}});
        }
        d.input->storage = d.storage;
        d.input_taken = true;
        return SceneRenderInput(std::move(d.input));
    }
} // namespace lux::scene
