#include <lux/engine/editor/scene/InstanceSpawnClient.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/ModelAsset.hpp>
#include <lux/engine/runtime/assets/AssetLoadSenders.hpp>
#include <lux/engine/editor/content/ModelMaterialResolve.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/runtime/execution/AsyncCallbackSender.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace lux::editor
{
    namespace ex = stdexec;

    namespace
    {
        using PreparedPlan = std::shared_ptr<InstanceSpawnPlan>;
        using PreparedOutcome = InstanceSpawnExp<PreparedPlan>;

        struct PreparedLoad final
        {
            PreparedPlan plan;
            std::optional<InstanceSpawnFailure> failure;
            lux::asset_runtime::LoadAssetBatch dependencies;
        };

        struct GpuNeed final
        {
            lux::asset::asset_id_t id{};
            lux::ecs::EResourceDomain domain{
                lux::ecs::EResourceDomain::MESH};
        };

        [[nodiscard]] InstanceSpawnFailure loadFailure(
            const lux::asset::asset_id_t& id,
            const lux::exec::AsyncFailure<lux::asset::EAssetError>& error)
            noexcept
        {
            return InstanceSpawnFailure{
                error.isRuntime()
                    ? EInstanceSpawnError::STOPPING
                    : EInstanceSpawnError::ASSET_LOAD_FAILED,
                id};
        }

        [[nodiscard]] lux::ecs::EResourceDomain materialDomain(
            const lux::asset::AssetManager& manager,
            const lux::asset::asset_id_t& id) noexcept
        {
            const auto* asset = manager.fetchAsset(id);
            return asset != nullptr &&
                    asset->type() ==
                        lux::asset::EAssetType::MATERIAL_INSTANCE
                ? lux::ecs::EResourceDomain::MATERIAL_INSTANCE
                : lux::ecs::EResourceDomain::MATERIAL;
        }

        class RequestTerminal final
        {
        public:
            explicit RequestTerminal(
                InstanceSpawnClient::Completion completion) noexcept
                : completion_(std::move(completion))
            {}

            void complete(InstanceSpawnOutcome outcome) noexcept
            {
                if (!completion_)
                    return;
                auto completion = std::move(completion_);
                completion(std::move(outcome));
            }

        private:
            InstanceSpawnClient::Completion completion_;
        };

        class GpuAwaitGroup final
        {
        public:
            using Deliver = lux::cxx::move_only_function<void(
                PreparedOutcome)>;

            GpuAwaitGroup(
                PreparedPlan plan,
                std::size_t count,
                Deliver deliver) noexcept
                : plan_(std::move(plan))
                , remaining_(count)
                , deliver_(std::move(deliver))
            {}

            void retainSelf(const std::shared_ptr<GpuAwaitGroup>& self)
            {
                self_keepalive_ = self;
            }

            void settle(
                const lux::asset::asset_id_t& id,
                std::uint64_t bits,
                const lux::ecs::ResourceFailure* failure) noexcept
            {
                if (settled_)
                    return;
                if (failure != nullptr || bits == 0u)
                {
                    finish(lux::cxx::unexpected(InstanceSpawnFailure{
                        EInstanceSpawnError::GPU_RESIDENCY_FAILED,
                        id}));
                    return;
                }
                if (--remaining_ == 0u)
                    finish(PreparedOutcome{std::move(plan_)});
            }

            void cancel() noexcept
            {
                if (settled_)
                    return;
                settled_ = true;
                waits_.clear();
                if (plan_)
                    plan_->residency_pins.clear();
                deliver_.reset();
                self_keepalive_.reset();
            }

            std::vector<lux::ecs::ResidencyCallbacks::Ticket> waits_;

        private:
            void finish(PreparedOutcome outcome) noexcept
            {
                if (settled_)
                    return;
                settled_ = true;
                waits_.clear();
                auto deliver = std::move(deliver_);
                self_keepalive_.reset();
                if (deliver)
                    deliver(std::move(outcome));
            }

            PreparedPlan plan_;
            std::size_t remaining_{0};
            bool settled_{false};
            Deliver deliver_;
            std::shared_ptr<GpuAwaitGroup> self_keepalive_;
        };
    }

    struct InstanceSpawnClient::State final
    {
        State(
            lux::exec::AsyncRuntime& runtime_value,
            lux::asset_runtime::AssetClient asset_client,
            lux::asset::AssetManager& manager_value,
            lux::ecs::ResidencyCallbacks residency_value,
            Commit commit_value)
            : runtime(&runtime_value)
            , assets(std::move(asset_client))
            , manager(&manager_value)
            , residency(std::move(residency_value))
            , commit(std::move(commit_value))
            , scope(std::make_unique<lux::exec::AsyncScope>(runtime_value))
        {}

        [[nodiscard]] PreparedLoad prepare(
            const lux::asset::asset_id_t& model_id,
            lux::exec::AsyncOutcome<lux::asset_runtime::LoadAsset> outcome)
            noexcept
        {
            PreparedLoad prepared;
            if (!outcome)
            {
                prepared.failure = loadFailure(model_id, outcome.error());
                prepared.dependencies = assets.loadBatchOperation(
                    std::span<const lux::asset::asset_id_t>{});
                return prepared;
            }

            const auto* model = manager->fetchAssetAs<lux::asset::ModelAsset>(
                model_id);
            if (model == nullptr)
            {
                prepared.failure = InstanceSpawnFailure{
                    EInstanceSpawnError::MODEL_NOT_FOUND,
                    model_id};
                prepared.dependencies = assets.loadBatchOperation(
                    std::span<const lux::asset::asset_id_t>{});
                return prepared;
            }
            if (model->meshAssetIds().empty())
            {
                prepared.failure = InstanceSpawnFailure{
                    EInstanceSpawnError::MODEL_EMPTY,
                    model_id};
                prepared.dependencies = assets.loadBatchOperation(
                    std::span<const lux::asset::asset_id_t>{});
                return prepared;
            }

            auto plan = std::make_shared<InstanceSpawnPlan>();
            plan->model = model_id;
            plan->model_revision = outcome->revision;
            if (model->data())
                plan->root_name = model->data()->name;
            if (model->skeletonAssetId())
                plan->skeleton = *model->skeletonAssetId();
            if (!model->animationClipAssetIds().empty())
                plan->animation_clip = model->animationClipAssetIds().front();

            const auto resolved = resolveModelSubmeshes(*model);
            plan->submeshes.reserve(model->meshAssetIds().size());
            std::vector<lux::asset::asset_id_t> dependencies;
            dependencies.reserve(model->meshAssetIds().size() * 2u + 2u);
            for (std::size_t index = 0;
                 index < model->meshAssetIds().size();
                 ++index)
            {
                const auto material = index < resolved.material.size()
                    ? resolved.material[index]
                    : lux::asset::asset_id_t{};
                plan->submeshes.push_back(InstanceSubmeshPlan{
                    model->meshAssetIds()[index],
                    material,
                    index < resolved.name.size()
                        ? resolved.name[index]
                        : std::string{}});
                dependencies.push_back(model->meshAssetIds()[index]);
                if (!material.is_nil())
                    dependencies.push_back(material);
            }
            if (!plan->skeleton.is_nil())
                dependencies.push_back(plan->skeleton);
            if (!plan->animation_clip.is_nil())
                dependencies.push_back(plan->animation_clip);

            prepared.dependencies = assets.loadBatchOperation(dependencies);
            prepared.plan = std::move(plan);
            return prepared;
        }

        [[nodiscard]] PreparedOutcome adoptDependencies(
            PreparedLoad prepared,
            lux::exec::AsyncOutcome<
                lux::asset_runtime::LoadAssetBatch> outcome) noexcept
        {
            if (prepared.failure)
                return lux::cxx::unexpected(*prepared.failure);
            if (!prepared.plan)
            {
                return lux::cxx::unexpected(InstanceSpawnFailure{
                    EInstanceSpawnError::MODEL_NOT_FOUND,
                    {}});
            }
            if (!outcome)
            {
                return lux::cxx::unexpected(loadFailure(
                    {},
                    outcome.error()));
            }
            prepared.plan->dependencies = std::move(outcome->assets);
            if (!revisionsCurrent(*prepared.plan))
            {
                return lux::cxx::unexpected(InstanceSpawnFailure{
                    EInstanceSpawnError::SUPERSEDED,
                    prepared.plan->model});
            }
            return std::move(prepared.plan);
        }

        [[nodiscard]] bool revisionsCurrent(
            const InstanceSpawnPlan& plan) const noexcept
        {
            if (manager->contentRevision(plan.model) != plan.model_revision)
                return false;
            return std::ranges::all_of(
                plan.dependencies,
                [this](const lux::asset_runtime::AssetLoadResult& dependency)
                {
                    return manager->contentRevision(dependency.id) ==
                        dependency.revision;
                });
        }

        [[nodiscard]] std::vector<GpuNeed> gpuNeeds(
            const InstanceSpawnPlan& plan) const
        {
            std::vector<GpuNeed> needs;
            needs.reserve(plan.submeshes.size() * 2u);
            const auto append = [&needs](GpuNeed need)
            {
                const bool duplicate = std::ranges::any_of(
                    needs,
                    [&need](const GpuNeed& existing)
                    {
                        return existing.id == need.id &&
                            existing.domain == need.domain;
                    });
                if (!duplicate)
                    needs.push_back(need);
            };
            for (const auto& submesh : plan.submeshes)
            {
                append(GpuNeed{
                    submesh.mesh,
                    lux::ecs::EResourceDomain::MESH});
                if (!submesh.material.is_nil())
                {
                    append(GpuNeed{
                        submesh.material,
                        materialDomain(*manager, submesh.material)});
                }
            }
            return needs;
        }

        [[nodiscard]] auto awaitResidency(PreparedOutcome prepared) noexcept
        {
            auto state = shared_from_this();
            return lux::exec::callbackSender<PreparedOutcome>(
                [state, prepared = std::move(prepared)](
                    auto completer) mutable noexcept
                    -> lux::exec::AsyncStopAction
                {
                    if (!prepared)
                    {
                        std::move(completer).complete(
                            std::move(prepared));
                        return {};
                    }
                    if (!state->accepting.load(std::memory_order_acquire))
                    {
                        std::move(completer).complete(
                            lux::cxx::unexpected(InstanceSpawnFailure{
                                EInstanceSpawnError::SCENE_CLOSED,
                                (*prepared)->model}));
                        return {};
                    }

                    auto plan = std::move(*prepared);
                    auto needs = state->gpuNeeds(*plan);
                    if (needs.empty())
                    {
                        std::move(completer).complete(
                            PreparedOutcome{std::move(plan)});
                        return {};
                    }

                    auto deliver = GpuAwaitGroup::Deliver{
                        [completer = std::move(completer)](
                            PreparedOutcome outcome) mutable noexcept
                        {
                            std::move(completer).complete(
                                std::move(outcome));
                        }};
                    auto group = std::make_shared<GpuAwaitGroup>(
                        plan,
                        needs.size(),
                        std::move(deliver));
                    group->retainSelf(group);
                    plan->residency_pins.reserve(needs.size());
                    group->waits_.reserve(needs.size());

                    const std::weak_ptr<GpuAwaitGroup> weak = group;
                    for (const auto& need : needs)
                    {
                        plan->residency_pins.push_back(
                            state->manager->acquire(need.id));
                        group->waits_.push_back(state->residency.await(
                            need.id,
                            [weak, id = need.id](
                                std::uint64_t bits,
                                const lux::ecs::ResourceFailure* failure)
                            {
                                if (auto locked = weak.lock())
                                    locked->settle(id, bits, failure);
                            }));
                    }
                    for (const auto& need : needs)
                        state->residency.request(need.id, need.domain);

                    return lux::exec::AsyncStopAction{
                        [group = std::move(group)]() mutable noexcept
                        {
                            // InstanceSpawnClient::close is main-thread only;
                            // AssetRef and residency tickets keep that contract.
                            group->cancel();
                        }};
                });
        }

        void finish(
            PreparedOutcome prepared,
            const std::shared_ptr<RequestTerminal>& terminal) noexcept
        {
            if (!prepared)
            {
                terminal->complete(lux::cxx::unexpected(prepared.error()));
                return;
            }
            if (!accepting.load(std::memory_order_acquire) || !commit)
            {
                terminal->complete(lux::cxx::unexpected(
                    InstanceSpawnFailure{
                        EInstanceSpawnError::SCENE_CLOSED,
                        (*prepared)->model}));
                return;
            }
            if (!revisionsCurrent(**prepared))
            {
                terminal->complete(lux::cxx::unexpected(
                    InstanceSpawnFailure{
                        EInstanceSpawnError::SUPERSEDED,
                        (*prepared)->model}));
                return;
            }

            const auto model = (*prepared)->model;
            const auto revision = (*prepared)->model_revision;
            const auto root = commit(std::move(**prepared));
            if (root == lux::meta::null_entity)
            {
                terminal->complete(lux::cxx::unexpected(
                    InstanceSpawnFailure{
                        EInstanceSpawnError::SCENE_CLOSED,
                        model}));
                return;
            }
            terminal->complete(InstanceSpawnResult{
                root,
                model,
                revision});
        }

        std::shared_ptr<State> shared_from_this()
        {
            return self.lock();
        }

        lux::exec::AsyncRuntime* runtime{nullptr};
        lux::asset_runtime::AssetClient assets;
        lux::asset::AssetManager* manager{nullptr};
        lux::ecs::ResidencyCallbacks residency;
        Commit commit;
        std::unique_ptr<lux::exec::AsyncScope> scope;
        std::atomic<bool> accepting{true};
        std::weak_ptr<State> self;
    };

    InstanceSpawnClient::InstanceSpawnClient(
        lux::exec::AsyncRuntime& runtime,
        lux::asset_runtime::AssetClient assets,
        lux::asset::AssetManager& manager,
        lux::ecs::ResidencyCallbacks residency,
        Commit commit)
        : state_(std::make_shared<State>(
              runtime,
              std::move(assets),
              manager,
              std::move(residency),
              std::move(commit)))
    {
        state_->self = state_;
    }

    InstanceSpawnClient::~InstanceSpawnClient()
    {
        if (state_ && state_->scope)
            state_->scope->requestStop();
    }

    lux::exec::AsyncSubmitResult InstanceSpawnClient::spawnModel(
        const lux::asset::asset_id_t& model,
        Completion completion)
    {
        if (!state_ || model.is_nil())
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::PAYLOAD_INVALID);
        }
        auto state = state_;
        if (!state->accepting.load(std::memory_order_acquire) ||
            !state->scope || !state->scope->isOpen())
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::STOPPING);
        }

        auto terminal = std::make_shared<RequestTerminal>(
            std::move(completion));
        auto pipeline = lux::asset_runtime::loadAsset(state->assets, model)
            | ex::continues_on(lux::exec::mainThreadScheduler(*state->runtime))
            | ex::then(
                  [state, model](
                      lux::exec::AsyncOutcome<
                          lux::asset_runtime::LoadAsset> outcome) noexcept
                  {
                      return state->prepare(model, std::move(outcome));
                  })
            | ex::let_value(
                  [state](PreparedLoad& prepared) noexcept
                  {
                      auto retained = std::move(prepared);
                      auto batch = std::move(retained.dependencies);
                      return lux::exec::execute(
                              state->assets.loadBatchClient(),
                              std::move(batch))
                          | ex::continues_on(
                                lux::exec::mainThreadScheduler(*state->runtime))
                          | ex::then(
                                [state,
                                 retained = std::move(retained)](
                                    lux::exec::AsyncOutcome<
                                        lux::asset_runtime::LoadAssetBatch>
                                        outcome) mutable noexcept
                                {
                                    return state->adoptDependencies(
                                        std::move(retained),
                                        std::move(outcome));
                                });
                  })
            | ex::let_value(
                  [state](PreparedOutcome& prepared) noexcept
                  {
                      return state->awaitResidency(std::move(prepared));
                  })
            | ex::continues_on(
                  lux::exec::mainThreadScheduler(*state->runtime))
            | ex::then(
                  [state, terminal](PreparedOutcome prepared) noexcept
                  {
                      state->finish(std::move(prepared), terminal);
                  })
            | ex::upon_error(
                  [terminal](auto&&) noexcept
                  {
                      terminal->complete(lux::cxx::unexpected(
                          InstanceSpawnFailure{
                              EInstanceSpawnError::STOPPING,
                              {}}));
                  })
            | ex::upon_stopped(
                  [terminal]() noexcept
                  {
                      terminal->complete(lux::cxx::unexpected(
                          InstanceSpawnFailure{
                              EInstanceSpawnError::STOPPING,
                              {}}));
                  });

        if (!lux::exec::spawn(*state->scope, std::move(pipeline)))
        {
            terminal->complete(lux::cxx::unexpected(
                InstanceSpawnFailure{
                    EInstanceSpawnError::STOPPING,
                    model}));
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::STOPPING);
        }
        return {};
    }

    lux::exec::AsyncScopeCloseSender
    InstanceSpawnClient::closeAsync() noexcept
    {
        if (!state_ || !state_->scope)
            return {};
        const bool first = state_->accepting.exchange(
            false,
            std::memory_order_acq_rel);
        if (first)
        {
            auto state = state_;
            lux::exec::detail::subscribeScopeClose(
                *state_->scope,
                [state]() mutable noexcept
                {
                    state->commit.reset();
                    state->self.reset();
                });
        }
        return state_->scope->closeAsync();
    }
}
