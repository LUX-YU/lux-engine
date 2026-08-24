#include <lux/engine/ecs/TaggedPropertyArchive.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::ecs
{
    namespace
    {
        template <class T>
        void appendPod(std::vector<std::byte>& destination, const T& value)
        {
            const std::size_t offset = destination.size();
            destination.resize(offset + sizeof(T));
            std::memcpy(destination.data() + offset, &value, sizeof(T));
        }

        template <class T>
        [[nodiscard]] bool readPod(
            std::span<const std::byte> bytes,
            std::size_t& offset,
            T& value
        ) noexcept
        {
            if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
                return false;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            offset += sizeof(T);
            return true;
        }
    } // namespace

    TaggedPropertyWriter::TaggedPropertyWriter(
        std::vector<std::byte>& destination,
        std::vector<std::string>& name_table
    ) noexcept
        : destination_(&destination), names_(&name_table)
    {
    }

    lux::cxx::expected<void, EComponentCodecError> TaggedPropertyWriter::write(
        std::string_view name,
        EComponentWireType type,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (finished_ || name.empty() || bytes.size() > std::numeric_limits<std::uint32_t>::max())
            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
        constexpr std::uint32_t header_size =
            sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint32_t);
        if (encoded_size_ > std::numeric_limits<std::uint32_t>::max() - header_size ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max() -
                encoded_size_ - header_size)
        {
            return lux::cxx::unexpected(EComponentCodecError::LIMIT_EXCEEDED);
        }
        try
        {
            auto iterator = std::find(names_->begin(), names_->end(), name);
            std::uint32_t name_index{};
            if (iterator == names_->end())
            {
                name_index = static_cast<std::uint32_t>(names_->size());
                names_->emplace_back(name);
            }
            else
                name_index = static_cast<std::uint32_t>(iterator - names_->begin());

            properties_.push_back(Property{
                name_index,
                type,
                std::vector<std::byte>(bytes.begin(), bytes.end())});
            last_payload_offset_ = encoded_size_ + header_size;
            encoded_size_ = last_payload_offset_ +
                static_cast<std::uint32_t>(bytes.size());
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EComponentCodecError::LIMIT_EXCEEDED);
        }
    }

    lux::cxx::expected<void, EComponentCodecError> TaggedPropertyWriter::finish() noexcept
    {
        if (finished_)
            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
        try
        {
            destination_->clear();
            appendPod(*destination_, static_cast<std::uint32_t>(properties_.size()));
            for (const Property& property : properties_)
            {
                appendPod(*destination_, property.name);
                appendPod(*destination_, static_cast<std::uint8_t>(property.type));
                appendPod(*destination_, static_cast<std::uint32_t>(property.bytes.size()));
                destination_->insert(
                    destination_->end(),
                    property.bytes.begin(),
                    property.bytes.end()
                );
            }
            finished_ = true;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EComponentCodecError::LIMIT_EXCEEDED);
        }
    }

    TaggedPropertyReader::TaggedPropertyReader(
        std::span<const std::byte> bytes,
        std::span<const std::string> name_table,
        TaggedPropertyLimits limits
    ) noexcept
        : bytes_(bytes), names_(name_table), limits_(limits)
    {
        if (!readPod(bytes_, offset_, remaining_) || remaining_ > limits_.max_properties)
            valid_ = false;
    }

    bool TaggedPropertyReader::next(EncodedPropertyView& property) noexcept
    {
        if (!valid_ || remaining_ == 0)
            return false;

        std::uint32_t name{};
        std::uint8_t type{};
        std::uint32_t size{};
        if (!readPod(bytes_, offset_, name) ||
            !readPod(bytes_, offset_, type) ||
            !readPod(bytes_, offset_, size) ||
            name >= names_.size() ||
            type > static_cast<std::uint8_t>(EComponentWireType::STABLE_REFERENCE) ||
            size > limits_.max_property_bytes ||
            offset_ > bytes_.size() || size > bytes_.size() - offset_)
        {
            valid_ = false;
            return false;
        }

        property.name = names_[name];
        property.type = static_cast<EComponentWireType>(type);
        property.bytes = bytes_.subspan(offset_, size);
        offset_ += size;
        --remaining_;
        if (remaining_ == 0 && offset_ != bytes_.size())
            valid_ = false;
        return true;
    }

    bool TaggedPropertyReader::valid() const noexcept
    {
        return valid_ && remaining_ == 0 && offset_ == bytes_.size();
    }
} // namespace lux::ecs
