#pragma once
#include "ProjectRequests.hpp"
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>

// Public SDK consumer: inspect an adopted Context in its signal, then close.
// No test-only pumping or incomplete Context construction.
namespace fixture
{
    template <class Assembly, class Inspect>
    void withContext(
        lux::editor::ProjectDescription description,
        lux::editor::ProjectManifest manifest,
        Assembly&& assembly,
        Inspect&& inspect
    )
    {
        using namespace lux;
        using namespace lux::editor;
        std::filesystem::create_directories(description.root);
        const auto file = description.root / (description.name + ".fixture.luxproj");
        assert(writeProjectManifestAtomic(file, manifest, EProjectWrite::REPLACE));
        auto made = LuxEngine::create(
            {"Installed Context consumer", 160, 120},
            EditorAssembly{std::forward<Assembly>(assembly)}
        );
        assert(made);
        auto host = std::move(*made);
        bool inspected{};
        auto changed = object::LuxObject::connect(
            host.get(),
            &LuxEngine::projectChanged,
            [&]() noexcept
            {
                if (host->project())
                {
                    assert(!inspected);
                    inspected = true;
                    inspect(*host->project());
                    host->window().exit();
                }
            }
        );
        auto failed = object::LuxObject::connect(
            host.get(),
            &LuxEngine::projectOpenFailed,
            [](const ProjectOpenFailure&) noexcept { std::abort(); }
        );
        assert(changed && failed && open(*host, file));
        assert(host->run() && inspected && !host->project());
    }
} // namespace fixture
