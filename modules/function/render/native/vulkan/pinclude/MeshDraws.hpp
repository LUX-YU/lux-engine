#pragma once

#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    // Context-owned mesh selection. Storage provides instance changes; this binding owns the
    // derived membership/indirect buffer. It is not a Storage or another resource registry.
    class MeshDraws final
    {
    public:
        static RenderResult<void>
        prepare(MeshInstanceStorage&, const MeshInstanceBinding&, PreparedStorageBinding&) noexcept;
        ~MeshDraws() noexcept;
        void added(std::uint32_t index) noexcept;
        void removed(std::uint32_t index) noexcept;

        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return positions_.size();
        }

        void disconnect() noexcept
        {
            storage_ = nullptr;
        }

    private:
        MeshDraws(DeviceSession&, MeshInstanceStorage&, MeshInstanceDrawGroup, PreparedStorageBinding&) noexcept;
        void changed() noexcept;
        DeviceSession& session_;
        MeshInstanceStorage* storage_;
        MeshInstanceDrawGroup selection_;
        PreparedStorageBinding& binding_;
        std::unique_ptr<Buffer> buffer_;
        RetirementReservation retirement_;
        std::vector<std::uint32_t> words_, positions_;
        std::size_t position_ = 0;
    };
} // namespace lux::render::vulkan
