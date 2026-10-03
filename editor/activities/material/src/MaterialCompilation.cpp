#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <atomic>
#include <thread>
namespace lux::editor::material
{
    struct detail::MaterialCompilation final
    {
        static std::shared_ptr<const CompiledMaterial> finish(MaterialCompileInputKey key,
            std::shared_ptr<const lux::material::MaterialSource> source, std::shared_ptr<const asset::MaterialAsset> artifact,
            lux::cxx::SharedBytes<> bytes)
        {
            return std::shared_ptr<const CompiledMaterial>(
                new CompiledMaterial(key, std::move(source), std::move(artifact), std::move(bytes))
            );
        }
    };

    namespace
    {
        std::atomic_uint64_t next_id{1};
        std::uint64_t allocateId() noexcept
        {
            auto value = next_id.load(std::memory_order_relaxed);
            while (value != UINT64_MAX)
                if (next_id.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return value;
            return 0;
        }
        template <class E> auto failed(E error)
        {
            return lux::cxx::unexpected(VMaterialCompileFailure{std::move(error)});
        }
        MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> compile(
            std::shared_ptr<const lux::material::MaterialSource> source,
            MaterialCompileInputKey key,
            std::stop_token stop
        )
        {
            if (stop.stop_requested())
                return failed(EMaterialCompileRequestError::CANCELLED);
            auto result = lux::material::compileMaterial(source->graph);
            if (!result)
                return failed(std::move(result.error()));
            auto asset = asset::MaterialAsset::create(
                {source->id, asset::MaterialAsset::asset_type},
                std::make_shared<const lux::rdesc::MaterialDescription>(std::move(*result))
            );
            if (!asset)
                return failed(asset.error());
            auto encoded = asset::TAssetSerDeser<asset::MaterialAsset>::encode(
                **asset,
                asset::AssetEncodeLimits{key.settings.byte_limit}
            );
            if (!encoded)
                return failed(encoded.error());
            auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
            return detail::MaterialCompilation::finish(
                key,
                std::move(source),
                std::move(*asset),
                lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes)
            );
        }
    }
    struct MaterialCompileOperation::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        MaterialCompileId id{allocateId()};
        MaterialCompileInputKey key;
        sessions::ObservationVersion observed;
        process::TaskId task_id;
        process::Task task;
        std::optional<MaterialCompileResult<std::shared_ptr<const CompiledMaterial>>> completed;
    };
    MaterialCompileOperation::MaterialCompileOperation(std::shared_ptr<Impl> state) : impl_(std::move(state)) {}
    MaterialCompileOperation::~MaterialCompileOperation()
    {
        cancel();
        // The executor keeps the completion's shared state alive. The unique public owner must
        // release its Task handle so abandoned UI delivery cannot form a record/state cycle.
        impl_->task = {};
    }
    MaterialCompileId MaterialCompileOperation::id() const noexcept
    {
        return impl_->id;
    }
    process::TaskId MaterialCompileOperation::task() const noexcept
    {
        return impl_->task_id;
    }
    MaterialCompileInputKey MaterialCompileOperation::key() const noexcept
    {
        return impl_->key;
    }
    sessions::ObservationVersion MaterialCompileOperation::observed() const noexcept
    {
        return impl_->observed;
    }
    bool MaterialCompileOperation::ready() const noexcept
    {
        return impl_->completed.has_value();
    }
    void MaterialCompileOperation::cancel() noexcept
    {
        impl_->task.requestStop();
    }
    MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> MaterialCompileOperation::result() const
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EMaterialCompileRequestError::WRONG_THREAD);
        if (!impl_->completed)
            return failed(EMaterialCompileRequestError::BUSY);
        return *impl_->completed;
    }
    MaterialCompileResult<std::unique_ptr<MaterialCompileOperation>> MaterialCompileOperation::start(
        process::ExecutionRuntime& execution,
        MaterialSnapshot snapshot,
        MaterialCompileSettings settings,
        std::uint64_t environment
    )
    {
        const MaterialCompileInputKey key{snapshot.content(), settings, environment};
        auto owned = std::make_shared<const MaterialSnapshot>(std::move(snapshot));
        auto source = std::shared_ptr<const lux::material::MaterialSource>(owned, &owned->source());
        const auto observed = owned->observed();
        const bool is_invalid_configuration = !key.settings.byte_limit || !key.settings.version;
        const bool is_invalid_environment = !key.environment;
        const bool is_invalid_request = is_invalid_configuration || is_invalid_environment;
        if (is_invalid_request)
            return failed(EMaterialCompileRequestError::INVALID_ID);
        auto state = std::make_shared<Impl>();
        state->key = key;
        state->observed = observed;
        if (!state->id.value)
            return failed(EMaterialCompileRequestError::CAPACITY);
        auto admitted = execution.submit(
            {"Compile material", "compiler"},
            [source = std::move(source), key, cpu = execution.cpu()](process::TaskReporter reporter) mutable noexcept {
                return stdexec::then(stdexec::schedule(cpu), [source = std::move(source), key, reporter] {
                    reporter.setPhase("Compile material");
                    return compile(source, key, reporter.stopToken());
                });
            },
            [state](process::TTaskResult<std::shared_ptr<const CompiledMaterial>, VMaterialCompileFailure>&& result
            ) noexcept {
                // Completion is an admitted leaf fact. No business admission, owner callback or guard release here.
                if (result)
                    state->completed.emplace(std::move(*result));
                else if (auto* error = result.error().domainFailure())
                    state->completed.emplace(lux::cxx::unexpected(std::move(*error)));
                else if (auto* error = result.error().executionFailure())
                    state->completed.emplace(failed(*error));
                else
                    state->completed.emplace(failed(EMaterialCompileRequestError::CANCELLED));
                state->task = {};
            }
        );
        if (!admitted)
            return failed(admitted.error());
        state->task_id = admitted->id();
        state->task = std::move(*admitted);
        return std::unique_ptr<MaterialCompileOperation>(new MaterialCompileOperation(std::move(state)));
    }
}
