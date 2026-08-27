#include <lux/engine/description/Shader.hpp>
#include <cstring>

namespace lux::rdesc
{
    Shader::Shader(const void* data, std::size_t size) : data_(size)
    {
        if (size != 0 && data != nullptr)
            std::memcpy(data_.data(), data, size);
    }

    const void* Shader::data() const noexcept
    {
        // Preserve the prior "null when empty" contract some callers test.
        return data_.empty() ? nullptr : data_.data();
    }
} // namespace lux::rdesc
