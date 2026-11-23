#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <algorithm>
#include <bit>

namespace lux::render
{
    FrameLayout::FrameLayout(const FrameLayout& other) noexcept : key_(other.key_), channels_(other.channels_)
    {
        ownTypeNames();
    }

    FrameLayout& FrameLayout::operator=(const FrameLayout& other) noexcept
    {
        if (this != &other) *this = FrameLayout{other};
        return *this;
    }

    FrameLayout::FrameLayout(FrameLayout&& other) noexcept
        : key_(std::exchange(other.key_, {})), channels_(std::move(other.channels_)),
          type_names_(std::move(other.type_names_))
    {
    }

    FrameLayout& FrameLayout::operator=(FrameLayout&& other) noexcept
    {
        if (this != &other)
        {
            key_ = std::exchange(other.key_, {});
            channels_ = std::move(other.channels_);
            type_names_ = std::move(other.type_names_);
        }
        return *this;
    }

    void FrameLayout::ownTypeNames() noexcept
    {
        type_names_.reserve(channels_.size());
        for (auto& channel : channels_)
        {
            const auto& names = type_names_.emplace_back(
                std::string{channel.declaration.name()}, std::string{channel.element.name()}
            );
            channel.declaration = {channel.declaration.hash(), names.declaration};
            channel.element = {channel.element.hash(), names.element};
        }
    }

    RenderResult<FrameLayout>
    FrameLayout::create(std::span<const FrameChannel> channels, std::uint64_t revision) noexcept
    {
        if (revision == 0) return cxx::unexpected(RenderError{kFrameContract});
        FrameLayout result;
        result.key_.input_layout_revision = revision;
        auto& qualified = result.channels_;
        qualified.reserve(channels.size());
        for (const auto& channel : channels)
        {
            const bool is_invalid_identity = channel.resource.name().empty() || !channel.declaration.isValid() ||
                !channel.element.isValid();
            const bool is_invalid_alignment = !std::has_single_bit(channel.alignment);
            const bool is_invalid_size = channel.size == 0 ||
                (!is_invalid_alignment && channel.size % channel.alignment != 0);
            const bool is_invalid_channel = is_invalid_identity || is_invalid_alignment || is_invalid_size;
            if (is_invalid_channel) return cxx::unexpected(RenderError{kFrameContract});
            bool found = false;
            for (const auto& previous : qualified)
            {
                const bool is_same_resource = previous.resource == channel.resource;
                const bool is_same_declaration = previous.declaration == channel.declaration;
                if (is_same_resource != is_same_declaration) return cxx::unexpected(RenderError{kFrameContract});
                if (!is_same_declaration || previous.element != channel.element) continue;
                if (previous != channel) return cxx::unexpected(RenderError{kFrameContract});
                found = true;
                break;
            }
            if (!found) qualified.push_back(channel);
        }
        result.ownTypeNames();
        return result;
    }

    bool FrameLayout::operator==(const FrameLayout& other) const noexcept
    {
        return revision() == other.revision() && std::ranges::equal(channels(), other.channels());
    }

    bool FrameLayout::hasSameChannels(const FrameLayout& other) const noexcept
    {
        if (channels().size() != other.channels().size()) return false;
        return std::ranges::all_of(channels(), [&](const auto& channel) noexcept
        {
            const auto index = other.find(channel.declaration, channel.element);
            return index < other.channels().size() && channel == other.channels()[index];
        });
    }

    std::size_t FrameLayout::find(cxx::basic_type_info declaration, cxx::basic_type_info element) const noexcept
    {
        const auto position = std::ranges::find_if(channels_, [&](const auto& channel) noexcept
        {
            return channel.declaration == declaration && channel.element == element;
        });
        return static_cast<std::size_t>(position - channels_.begin());
    }
} // namespace lux::render
