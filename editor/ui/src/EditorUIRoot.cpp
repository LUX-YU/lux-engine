#include <algorithm>
#include <exception>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/editor/EditorUIRoot.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    namespace
    {
        FrameworkFailure attachmentFailure(ui::EAttachmentError error)
        {
            const auto code = error == ui::EAttachmentError::BUSY ? EFrameworkError::BUSY : EFrameworkError::UI;
            return {code, "UI attachment refused", static_cast<std::uint64_t>(error)};
        }
        struct Mutation final
        {
            bool& active;
            explicit Mutation(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Mutation()
            {
                active = false;
            }
        };
    } // namespace
    struct EditorUIRoot::Impl final
    {
        cxx::AutoSparseSet<std::unique_ptr<ui::Pane>> owners;
        Capture* capture{};
        bool mutating{};
    };
    EditorUIRoot::EditorUIRoot() noexcept : Root(), impl_(std::make_unique<Impl>())
    {
    }
    EditorUIRoot::~EditorUIRoot() noexcept
    {
        if (!clearProjectUi())
        {
            std::terminate(); // Destruction from a callback violates the Root lifetime contract.
        }
    }
    FrameworkResult<std::unique_ptr<EditorUIRoot>> EditorUIRoot::create(
        ui::RootConfig config
    ) noexcept
    {
        auto root = std::unique_ptr<EditorUIRoot>{new EditorUIRoot()};
        auto initialized = root->initialize(std::move(config));
        if (!initialized)
        {
            return cxx::unexpected(FrameworkFailure{
                EFrameworkError::UI,
                "Root initialization failed",
                static_cast<std::uint64_t>(initialized.error())
            });
        }
        return root;
    }
    FrameworkResult<void> EditorUIRoot::checkStructureSafe() noexcept
    {
        if (impl_->mutating)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::BUSY, "Project UI is changing"});
        }
        auto prepared = prepareDetach(std::span<ui::Pane* const>{});
        if (!prepared)
        {
            return cxx::unexpected(attachmentFailure(prepared.error()));
        }
        return {};
    }
    FrameworkResult<void> EditorUIRoot::mountProjectUi(std::span<std::unique_ptr<ui::Pane>> candidates) noexcept
    {
        if (auto safe = checkStructureSafe(); !safe)
        {
            return safe;
        }
        Mutation guard{impl_->mutating};
        std::vector<ui::Pane*> panes;
        panes.reserve(candidates.size());
        for (auto& owner : candidates)
        {
            panes.push_back(owner.get());
        }
        auto prepared = prepareMount(panes);
        if (!prepared)
        {
            return cxx::unexpected(attachmentFailure(prepared.error()));
        }
        std::vector<std::size_t> slots;
        slots.reserve(candidates.size());
        for (std::size_t index = 0; index != candidates.size(); ++index)
        {
            slots.push_back(impl_->owners.emplace(nullptr));
        }
        auto adopt = [&]() noexcept
        {
            for (std::size_t index = 0; index != candidates.size(); ++index)
            {
                *impl_->owners.tryGet(slots[index]) = std::move(candidates[index]);
            }
        };
        auto committed = commit(*prepared, adopt);
        if (!committed)
        {
            for (auto slot : slots)
            {
                impl_->owners.erase(slot);
            }
            return cxx::unexpected(attachmentFailure(committed.error()));
        }
        return {};
    }
    FrameworkResult<ui::PaneHandle> EditorUIRoot::takePane(std::unique_ptr<ui::Pane>& candidate) noexcept
    {
        auto* pane = candidate.get();
        auto result = mountProjectUi(std::span{&candidate, 1});
        if (!result)
        {
            return cxx::unexpected(std::move(result.error()));
        }
        auto handle = identify(*pane);
        if (!handle)
        {
            std::terminate(); // Successful mount establishes this identity before notifying.
        }
        return *handle;
    }
    ui::Pane* EditorUIRoot::projectPane(const ui::PaneHandle& handle) const noexcept
    {
        auto found = findPane(handle);
        if (!found)
        {
            return nullptr;
        }
        for (const auto& owner : impl_->owners.values())
        {
            if (owner.get() == *found)
            {
                return owner.get();
            }
        }
        return nullptr;
    }
    std::size_t EditorUIRoot::projectPaneCount() const noexcept
    {
        return impl_->owners.size();
    }
    FrameworkResult<void> EditorUIRoot::removePane(const ui::PaneHandle& handle) noexcept
    {
        if (auto safe = checkStructureSafe(); !safe)
        {
            return safe;
        }
        Mutation guard{impl_->mutating};
        auto* pane = projectPane(handle);
        if (!pane)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::NOT_FOUND, "Pane is not project owned"});
        }
        auto prepared = prepareDetach(*pane);
        if (!prepared)
        {
            return cxx::unexpected(attachmentFailure(prepared.error()));
        }
        const auto found = std::ranges::find_if(
            impl_->owners.keys(),
            [&](auto key) { return impl_->owners.tryGet(key)->get() == pane; }
        );
        const auto key = *found;
        std::unique_ptr<ui::Pane> retiring;
        auto release = [&]() noexcept
        {
            // No destructor runs inside SparseSet swap-and-pop or the attachment notification.
            retiring = std::move(*impl_->owners.tryGet(key));
            impl_->owners.erase(key);
        };
        auto detached = commit(*prepared, release);
        if (!detached)
        {
            return cxx::unexpected(attachmentFailure(detached.error()));
        }
        return {};
    }
    FrameworkResult<void> EditorUIRoot::clearProjectUi() noexcept
    {
        if (auto safe = checkStructureSafe(); !safe)
        {
            return safe;
        }
        Mutation guard{impl_->mutating};
        std::vector<ui::Pane*> panes;
        std::vector<std::unique_ptr<ui::Pane>> retiring(impl_->owners.size());
        for (const auto& owner : impl_->owners.values())
        {
            panes.push_back(owner.get());
        }
        auto prepared = prepareDetach(panes);
        if (!prepared)
        {
            return cxx::unexpected(attachmentFailure(prepared.error()));
        }
        auto release = [&]() noexcept
        {
            std::size_t index{};
            for (auto key : impl_->owners.keys())
            {
                retiring[index++] = std::move(*impl_->owners.tryGet(key));
            }
            impl_->owners.clear();
        };
        auto detached = commit(*prepared, release);
        if (!detached)
        {
            return cxx::unexpected(attachmentFailure(detached.error()));
        }
        // Destroy nodes only after container mutation and all attachment callbacks have returned.
        while (!retiring.empty())
        {
            retiring.pop_back();
        }
        return {};
    }
    cxx::expected<void, ui::ECaptureError> EditorUIRoot::frame(
        ui::FrameInfo info,
        ui::DrawData* data,
        Capture capture
    ) noexcept
    {
        if (impl_->capture)
        {
            return cxx::unexpected(ui::ECaptureError::FRAME_OPEN);
        }
        impl_->capture = &capture;
        auto result = update(info, data);
        impl_->capture = nullptr;
        return result;
    }
    cxx::expected<void, ui::ECaptureError> EditorUIRoot::drawDataReady(const ui::DrawData& data) noexcept
    {
        return impl_->capture ? (*impl_->capture)(data) : cxx::expected<void, ui::ECaptureError>{};
    }
} // namespace lux::editor
