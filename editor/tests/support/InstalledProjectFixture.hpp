#pragma once
#include <cassert>
#include <chrono>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <thread>

// SDK consumers use the real public product opening path, never a source-private Context constructor.
namespace fixture
{
    class ContextOwner final
    {
    public:
        explicit ContextOwner(std::unique_ptr<lux::editor::LuxEngine> host) noexcept : host_(std::move(host)) {}
        ContextOwner(ContextOwner&&) noexcept = default;
        ContextOwner& operator=(ContextOwner&&) noexcept = default;
        [[nodiscard]] lux::editor::EditorContext& operator*() const noexcept
        {
            return *host_->context();
        }
        [[nodiscard]] lux::editor::EditorContext* operator->() const noexcept
        {
            return host_->context();
        }
        void reset() noexcept
        {
            host_.reset();
        }

    private:
        std::unique_ptr<lux::editor::LuxEngine> host_;
    };

    template <class Assembly>
    auto createContext(
        lux::engine::EngineContext&,
        lux::editor::ProjectDescription description,
        lux::editor::ProjectManifest manifest,
        Assembly&& assembly
    ) noexcept -> lux::editor::FrameworkResult<ContextOwner>
    {
        using namespace lux;
        using namespace lux::editor;
        if (auto registered = registerFrameworkErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        const auto file = description.root / (description.name + ".fixture.luxproj");
        std::filesystem::create_directories(description.root);
        assert(writeProjectManifestAtomic(file, manifest, EProjectWrite::REPLACE));
        auto host = LuxEngine::create(
            {"Installed Context consumer", 160, 120},
            EditorAssembly{std::forward<Assembly>(assembly)}
        );
        assert(host);
        assert((*host)->openProject(file));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            const auto state = (*host)->projectStatus();
            if (state.state == EProjectTransition::SUCCEEDED)
            {
                assert((*host)->context());
                return ContextOwner{std::move(*host)};
            }
            if (state.state == EProjectTransition::FAILED)
            {
                return cxx::unexpected(state.failure);
            }
            assert((*host)->frame());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
} // namespace fixture
