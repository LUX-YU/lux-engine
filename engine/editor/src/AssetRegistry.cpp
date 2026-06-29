#include <lux/engine/editor/AssetRegistry.hpp>

#include <cstdio>
#include <system_error>
#include <utility>

namespace lux::editor
{
    void AssetRegistry::scan(const std::filesystem::path& content_root)
    {
        root_ = content_root;
        assets_.clear();

        // The registry is a VIEW over the same LooseDirProvider the editor
        // mounts at /Game — one path->id table, not a second scan. Keep the
        // provider instance stable across refresh() so the AssetVfs mount
        // stays valid; only swap it when the root actually changes.
        if (!provider_ || provider_->rootDir() != content_root)
        {
            provider_ =
                std::make_shared<lux::asset::LooseDirProvider>(content_root);
        }
        provider_->rescan();

        provider_->enumerate(
            [&](const lux::asset::ProviderEntry& e)
            {
                AssetMeta m;
                m.id   = e.id;
                m.type = e.type;
                if (auto file = provider_->filePathOf(e.id))
                {
                    m.name = file->stem().string();
                    std::error_code rel_ec;
                    m.rel_path = std::filesystem::relative(
                        *file, content_root, rel_ec).generic_string();
                }
                else
                {
                    // Provider entries always have a backing file today;
                    // derive from the vpath if that ever changes.
                    m.name     = std::string{
                        e.vpath.substr(e.vpath.rfind('/') + 1) };
                    m.rel_path = e.vpath;
                }
                assets_.push_back(std::move(m));
            }
        );

        // Authoring hazards the provider resolved deterministically (vpath /
        // uuid collisions, case clashes that cook will reject) — surface them
        // where the editor log is read.
        for (const auto& d : provider_->diagnostics())
            std::fprintf(stderr, "[AssetRegistry] %s\n", d.c_str());
    }

    std::vector<std::uint32_t> AssetRegistry::ofType(lux::asset::EAssetType type) const
    {
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(assets_.size()); ++i)
            if (assets_[i].type == type)
                out.push_back(i);
        return out;
    }
}
