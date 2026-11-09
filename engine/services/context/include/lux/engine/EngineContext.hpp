#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>
#include <memory>
#include <functional>
#include <vector>

namespace lux::execution
{
    class ExecutionRuntime;
    struct ExecutionRuntimeConfig;
}

namespace lux::object { class ObjectRuntime; }
namespace lux::asset
{
    class AssetVfs;
    class AssetCodec;
    struct AssetDecodeLimits;
}

namespace lux::engine::assets
{
    class AssetRepository;
    class AssetRetentionPolicy;
}

#if defined(LUX_ENGINE_WITH_RENDER)
namespace lux::render
{
    class RenderServer;
    class RenderClient;
    struct RenderServerConfig;
    struct DeviceOptions;
}
#endif

namespace lux::engine
{
    class Engine;
    class EngineContext;
    using InitResult = cxx::expected<std::unique_ptr<EngineContext>, error::Error>;

    // Owns process facilities. Destroy on Main, outside dispatch, after all consumers have retired.
    // Registered asset providers and external callback targets outlive repository settlement.
    class EngineContext final
    {
    public:
        ~EngineContext() noexcept;
        EngineContext(const EngineContext&) = delete;
        EngineContext& operator=(const EngineContext&) = delete;
        EngineContext(EngineContext&&) = delete;
        EngineContext& operator=(EngineContext&&) = delete;

        [[nodiscard]] execution::ExecutionRuntime& execution() noexcept;
        [[nodiscard]] object::ObjectRuntime& objects() noexcept;
        [[nodiscard]] asset::AssetVfs& assetVfs() noexcept;
        [[nodiscard]] assets::AssetRepository& assets() noexcept;
#if defined(LUX_ENGINE_WITH_RENDER)
        // Borrowed through Context lifetime; request and response delivery belong to Main safe points.
        [[nodiscard]] lux::render::RenderClient& renderClient() noexcept;
#endif

    private:
        friend class Engine;
#if defined(LUX_ENGINE_WITH_RENDER)
        friend InitResult init(
            execution::ExecutionRuntimeConfig execution,
            asset::AssetDecodeLimits asset_limits,
            lux::render::RenderServerConfig rendering,
            lux::render::DeviceOptions device,
            std::unique_ptr<assets::AssetRetentionPolicy> retention,
            std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs,
            std::function<cxx::expected<void, error::Error>(lux::render::RenderServer&)> register_render
        ) noexcept;
#else
        friend InitResult init(
            execution::ExecutionRuntimeConfig execution,
            asset::AssetDecodeLimits asset_limits,
            std::unique_ptr<assets::AssetRetentionPolicy> retention,
            std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs
        ) noexcept;
#endif
        struct Impl;
        explicit EngineContext(std::unique_ptr<Impl> impl) noexcept;
        void bindEngineStages(Engine& engine) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine
