#include "ModelSceneSupport.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>

#include <cstdio>
#include <fstream>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/systems/render/WindowSurface.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <numbers>

namespace lux::engine::test
{
    namespace
    {
        error::Error modelFailure(const char* message) noexcept
        {
            std::fprintf(stderr, "Model preparation: %s\n", message);
            return {render::kRuntimeState};
        }
    } // namespace

    Result runTask(exec::task<Result> task, bool window_events) noexcept
    {
        auto& execution = execution::ExecutionRuntime::instance();
        std::optional<Result> completed;
        auto normalized =
            stdexec::starts_on(execution.mainScheduler(), std::move(task)) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
            stdexec::upon_stopped(
                []() noexcept -> Result { return cxx::unexpected(error::Error{render::kCommandCancelled}); }
            );
        auto accepted = execution.spawn(stdexec::then(
            std::move(normalized),
            [&completed](Result result) noexcept { completed.emplace(std::move(result)); }
        ));
        if (!accepted)
        {
            return cxx::unexpected(execution::toError(accepted.error()));
        }
        if (!window_events)
        {
            auto settled = execution.waitUntil([&completed]() noexcept { return completed.has_value(); });
            if (!settled) return cxx::unexpected(execution::toError(settled.error()));
            return std::move(*completed);
        }
        while (!completed)
        {
            window::LuxWindow::pollEvents();
            if (!execution.dispatch())
            {
                std::abort();
            }
            if (!completed && !execution.hasReadyWork())
            {
                window::LuxWindow::waitEvents(0.02);
            }
        }
        return std::move(*completed);
    }

    render::RenderResult<ecs::Entity> createViewComponents(
        ecs::Registry& registry,
        window::LuxWindow& window,
        const render::TargetDesc& description,
        bool orthographic
    ) noexcept
    {
        const auto entity = registry.create();
        scene::ViewComponent view;
        view.projection = orthographic
                              ? scene::VProjection{scene::OrthographicProjection{3.2, 0.1, 100}}
                              : scene::VProjection{scene::PerspectiveProjection{std::numbers::pi / 3, 0.1, 100}};
        view.background = {0.05F, 0.08F, 0.13F, 1};
        ecs::Transform3D transform;
        transform.translation = {0, 0.5, 3.7};
        const Eigen::Vector3d forward = -transform.translation.normalized();
        const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
        Eigen::Matrix3d basis;
        basis.col(0) = right;
        basis.col(1) = right.cross(forward);
        basis.col(2) = -forward;
        transform.rotation = Eigen::Quaterniond(basis);
        rendering::RenderTargetComponent target;
        target.description = description;
        window.framebufferSize(target.description.width, target.description.height);
        if (target.description.kind == render::ETargetKind::SURFACE)
        {
            render::SurfaceDescription surface;
            if (!rendering::makeWindowSurface(window.nativeWindow(), true, surface))
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            target.surface = surface;
        }
        registry.emplace<scene::ViewComponent>(entity, view);
        registry.emplace<ecs::Transform3D>(entity, transform);
        registry.emplace<rendering::RenderTargetComponent>(entity, target);
        return entity;
    }

    exec::task<Result> resolveComposition(
        render::RenderClient& client,
        render::CapabilityComposition& effect,
        render::EffectOutputDesc output,
        const render::DeferredLightingConfiguration& lighting
    ) noexcept
    {
        auto composition = render::deferredRenderComposition(lighting.clustered, lighting.shadows);
        for (auto& contribution : composition.contributions)
        {
            if (contribution.capability == render::kDeferredLightingCapability)
            {
                auto encoded = render::encodeDeferredLightingConfiguration(lighting);
                if (!encoded) co_return cxx::unexpected(encoded.error());
                contribution.configuration = std::move(*encoded);
            }
            auto choices =
                co_await client.request<lux::render::ListCapabilities>(
                    {contribution.capability, contribution.implementation, output}
                );
            if (choices.error.type || choices.implementations.size() != 1)
            {
                co_return cxx::unexpected(modelFailure("capability implementation lookup"));
            }
            auto& selected = choices.implementations.front();
            if (selected.unavailable.type)
            {
                co_return cxx::unexpected(selected.unavailable);
            }
            effect.contributions.push_back(
                {contribution.key, selected.capability.type, selected.implementation_id,
                 std::move(contribution.configuration)}
            );
        }
        effect.outputs = std::move(composition.outputs);
        co_return Result{};
    }

    exec::task<render::RenderResult<std::shared_ptr<asset::DirectoryAssetProvider>>>
    openModelAssets(const std::filesystem::path& directory) noexcept
    {
        auto& execution = execution::ExecutionRuntime::instance();
        auto io = execution.blockingIoScheduler();
        if (!io)
        {
            co_return cxx::unexpected(execution::toError(io.error()));
        }
        auto opened = co_await stdexec::continues_on(
            stdexec::then(
                stdexec::schedule(*io),
                [&]() noexcept { return asset::DirectoryAssetProvider::create(directory); }
            ),
            execution.mainScheduler()
        );
        if (!opened)
        {
            co_return cxx::unexpected(modelFailure("open cooked asset directory"));
        }
        co_return std::move(*opened);
    }

    exec::task<render::RenderResult<toolchain::ModelCookProduct>>
    cookModelAssets(const std::filesystem::path& source, const std::filesystem::path& directory) noexcept
    {
        const auto begin = std::chrono::steady_clock::now();
        double source_read_ms = 0, cook_ms = 0;
        auto& execution = execution::ExecutionRuntime::instance();
        auto io = execution.blockingIoScheduler();
        if (!io)
        {
            co_return cxx::unexpected(execution::toError(io.error()));
        }
        toolchain::ModelSource capture;
        capture.entry = source.filename().generic_string();
        std::vector<std::string> requested{capture.entry};
        uuids::uuid_name_generator ids{*uuids::uuid::from_string("c45aaf74-31ec-4225-bd80-55875831e408")};
        const asset::AssetId model_id{ids(std::filesystem::absolute(source).generic_string())};
        toolchain::ModelCookProduct product;
        for (;;)
        {
            const auto read_begin = std::chrono::steady_clock::now();
            auto files = co_await stdexec::continues_on(
                stdexec::then(
                    stdexec::schedule(*io),
                    [&]() noexcept { return toolchain::readModelSourceFiles(source.parent_path(), requested); }
                ),
                execution.mainScheduler()
            );
            if (!files)
            {
                co_return cxx::unexpected(modelFailure(files.error().detail.c_str()));
            }
            source_read_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - read_begin
            ).count();
            for (auto& file : *files)
            {
                capture.files.push_back(std::move(file));
            }
            const auto cook_begin = std::chrono::steady_clock::now();
            auto attempt = co_await stdexec::continues_on(
                stdexec::then(
                    stdexec::schedule(execution.cpuScheduler()),
                    [&]() { return toolchain::cookModel({model_id, asset::ModelAsset::asset_type}, capture); }
                ),
                execution.mainScheduler()
            );
            cook_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cook_begin
            ).count();
            if (auto* cooked = std::get_if<toolchain::ModelCookProduct>(&attempt))
            {
                product = std::move(*cooked);
                break;
            }
            if (const auto* error = std::get_if<toolchain::ModelCookFailure>(&attempt))
            {
                co_return cxx::unexpected(modelFailure(error->detail.c_str()));
            }
            requested = std::move(std::get<toolchain::ModelSourceRequests>(attempt).paths);
        }
        using Encoded = std::vector<std::pair<std::string, std::vector<std::byte>>>;
        const auto encode_begin = std::chrono::steady_clock::now();
        auto encoded = co_await stdexec::continues_on(
            stdexec::then(
                stdexec::schedule(execution.cpuScheduler()),
                [&]() noexcept -> render::RenderResult<Encoded>
                {
                    Encoded files;
                    const auto append = [&files]<class Asset>(const Asset& asset, std::string filename) noexcept
                    {
                        auto bytes = asset::TAssetSerDeser<std::remove_cv_t<Asset>>::encode(
                            asset,
                            asset::AssetEncodeLimits{64U << 20U}
                        );
                        if (!bytes)
                        {
                            return false;
                        }
                        files.emplace_back(std::move(filename), std::move(*bytes));
                        return true;
                    };
                    for (std::size_t i = 0; i < product.meshes.size(); ++i)
                    {
                        auto name = i == 0 ? std::string{"mesh.luxasset"} : "mesh_" + std::to_string(i) + ".luxasset";
                        if (!append(*product.meshes[i], std::move(name)))
                        {
                            return cxx::unexpected(modelFailure("encode mesh"));
                        }
                    }
                    for (std::size_t i = 0; i < product.materials.size(); ++i)
                    {
                        if (!append(*product.materials[i], "material_" + std::to_string(i) + ".luxasset"))
                        {
                            return cxx::unexpected(modelFailure("encode material"));
                        }
                    }
                    for (std::size_t i = 0; i < product.textures.size(); ++i)
                    {
                        if (!append(*product.textures[i], "texture_" + std::to_string(i) + ".luxasset"))
                        {
                            return cxx::unexpected(modelFailure("encode texture"));
                        }
                    }
                    if (product.skeleton && !append(**product.skeleton, "skeleton.luxasset"))
                    {
                        return cxx::unexpected(modelFailure("encode skeleton"));
                    }
                    for (std::size_t i = 0; i < product.animations.size(); ++i)
                    {
                        if (!append(*product.animations[i], "animation_" + std::to_string(i) + ".luxasset"))
                        {
                            return cxx::unexpected(modelFailure("encode animation"));
                        }
                    }
                    // The root is written after its children. Cooking uses a dedicated, offline output directory.
                    if (!append(*product.model, "model.luxasset"))
                    {
                        return cxx::unexpected(modelFailure("encode model"));
                    }
                    return files;
                }
            ),
            execution.mainScheduler()
        );
        if (!encoded)
        {
            co_return cxx::unexpected(encoded.error());
        }
        const auto write_begin = std::chrono::steady_clock::now();
        auto written = co_await stdexec::continues_on(
            stdexec::then(
                stdexec::schedule(*io),
                [&]() noexcept -> Result
                {
                    std::error_code error;
                    std::filesystem::create_directories(directory, error);
                    if (error)
                    {
                        return cxx::unexpected(modelFailure("create cooked directory"));
                    }
                    for (const auto& [name, bytes] : *encoded)
                    {
                        std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
                        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        output.close();
                        if (!output)
                        {
                            return cxx::unexpected(modelFailure("write cooked asset"));
                        }
                    }
                    return {};
                }
            ),
            execution.mainScheduler()
        );
        if (!written)
        {
            co_return cxx::unexpected(written.error());
        }
        const auto end = std::chrono::steady_clock::now();
        std::printf(
            "COOK_RESULT source=%s read_ms=%.3f import_cook_ms=%.3f encode_ms=%.3f write_ms=%.3f total_ms=%.3f "
            "meshes=%zu materials=%zu textures=%zu animations=%zu\n",
            source.filename().string().c_str(), source_read_ms, cook_ms,
            std::chrono::duration<double, std::milli>(write_begin - encode_begin).count(),
            std::chrono::duration<double, std::milli>(end - write_begin).count(),
            std::chrono::duration<double, std::milli>(end - begin).count(),
            product.meshes.size(), product.materials.size(), product.textures.size(), product.animations.size()
        );
        co_return std::move(product);
    }
} // namespace lux::engine::test
