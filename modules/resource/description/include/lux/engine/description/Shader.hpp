#pragma once
#include <lux/engine/resource/visibility.h>
#include <cstddef>
#include <vector>

namespace lux::rdesc
{
    /// Owns a shader's raw payload (SPIR-V bytes). Backed by a
    /// std::vector<std::byte> so copy/move/destroy are correct by
    /// construction (rule of zero) — no hand-written special members.
    class LUX_RESOURCE_PUBLIC Shader
    {
    public:
        Shader() = default;

        /// Take ownership of an existing byte buffer (no copy).
        explicit Shader(std::vector<std::byte> bytes) noexcept : data_(std::move(bytes))
        {
        }

        /// Copy `size` bytes from `data` into owned storage. `data` is
        /// borrowed — the caller keeps ownership of its buffer.
        Shader(const void* data, std::size_t size);

        [[nodiscard]] const void* data() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept
        {
            return data_.size();
        }

    private:
        std::vector<std::byte> data_;
    };
}
