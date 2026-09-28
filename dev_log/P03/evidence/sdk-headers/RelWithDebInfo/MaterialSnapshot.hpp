#pragma once

#include <lux/engine/editor/material/MaterialEdit.hpp>
#include <lux/engine/editor/sessions/SessionState.hpp>

namespace lux::editor::material
{
    struct MaterialSnapshotBudget final
    {
        std::size_t max_bytes{64 * 1024 * 1024};
    };
    class MaterialSession;

    class MaterialSnapshot final
    {
    public:
        MaterialSnapshot() = default;
        MaterialSnapshot(MaterialSnapshot&&) noexcept = default;
        MaterialSnapshot& operator=(MaterialSnapshot&& other) noexcept
        {
            MaterialSnapshot previous(std::move(other));
            using std::swap;
            swap(code_, previous.code_);
            swap(source_, previous.source_);
            swap(content_, previous.content_);
            swap(observed_, previous.observed_);
            return *this;
        }
        MaterialSnapshot(const MaterialSnapshot&) = delete;
        MaterialSnapshot& operator=(const MaterialSnapshot&) = delete;
        [[nodiscard]] const lux::material::MaterialSource& source() const noexcept
        {
            return source_;
        }
        [[nodiscard]] sessions::ContentStamp content() const noexcept
        {
            return content_;
        }
        [[nodiscard]] sessions::ObservationVersion observed() const noexcept
        {
            return observed_;
        }

    private:
        friend class MaterialSession;
        friend class MaterialReadView;
        // Kept outside the graph, so the last node destructor returns before code is released.
        std::vector<contracts::CodeLease> code_;
        lux::material::MaterialSource source_;
        sessions::ContentStamp content_;
        sessions::ObservationVersion observed_;
    };

    // Synchronous borrow invalidated by edit/reload/close. Owning clone/codec calls enter the same
    // gate as capture. Inspecting a const node never exposes a writable node through unique_ptr.
    class MaterialReadView final
    {
    public:
        template <class Fn>
        [[nodiscard]] auto withRead(Fn&& function) const
            -> std::invoke_result_t<Fn, const lux::material::MaterialSource&>
        {
            return gate_.withRead([&] { return std::invoke(std::forward<Fn>(function), std::as_const(source_)); });
        }
        [[nodiscard]] MaterialEditResult<MaterialSnapshot> capture(MaterialSnapshotBudget budget = {}) const;
        [[nodiscard]] MaterialEditResult<std::string> encode() const;

    private:
        friend class MaterialSession;
        MaterialReadView(
            const lux::material::MaterialSource& source,
            const std::vector<contracts::CodeLease>& code,
            sessions::ContentStamp content,
            sessions::ObservationVersion observed,
            sessions::EditGate& gate
        ) noexcept
            : source_(source), code_(code), content_(content), observed_(observed), gate_(gate)
        {}
        const lux::material::MaterialSource& source_;
        const std::vector<contracts::CodeLease>& code_;
        sessions::ContentStamp content_;
        sessions::ObservationVersion observed_;
        sessions::EditGate& gate_;
    };
}
