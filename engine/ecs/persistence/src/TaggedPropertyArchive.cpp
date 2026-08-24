#include <lux/engine/ecs/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/persistence/detail/LittleEndian.hpp>

#include <algorithm>
#include <limits>

namespace lux::ecs
{
    namespace
    {
        using persistence::detail::appendLittle;
        using persistence::detail::patchU32;
        using persistence::detail::readLittle;
    } // namespace

    TaggedPropertyWriter::TaggedPropertyWriter(
        std::vector<std::byte>& destination,
        std::vector<std::string>& name_table
    ) noexcept
        : destination_(&destination), names_(&name_table)
    {
        destination_->clear();
        try
        {
            appendLittle(*destination_, std::uint32_t{});
        }
        catch (...)
        {
            destination_->clear();
            allocation_failed_ = true;
        }
    }

    lux::cxx::expected<void, EComponentCodecError> TaggedPropertyWriter::write(
        std::string_view name,
        EComponentWireType type,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (allocation_failed_)
        {
            return lux::cxx::unexpected(
                EComponentCodecError::ALLOCATION_FAILURE
            );
        }
        if (finished_ || name.empty() ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
        }
        if (property_count_ == std::numeric_limits<std::uint32_t>::max())
            return lux::cxx::unexpected(EComponentCodecError::LIMIT_EXCEEDED);

        constexpr std::size_t header_size =
            sizeof(std::uint32_t) + sizeof(std::uint8_t) +
            sizeof(std::uint32_t);
        if (destination_->size() >
                std::numeric_limits<std::uint32_t>::max() - header_size ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max() -
                destination_->size() - header_size)
        {
            return lux::cxx::unexpected(EComponentCodecError::LIMIT_EXCEEDED);
        }

        const std::size_t destination_size = destination_->size();
        bool inserted_name{};
        try
        {
            auto iterator = std::find(names_->begin(), names_->end(), name);
            std::uint32_t name_index{};
            if (iterator == names_->end())
            {
                if (names_->size() >=
                    std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(
                        EComponentCodecError::LIMIT_EXCEEDED
                    );
                }
                name_index = static_cast<std::uint32_t>(names_->size());
                names_->emplace_back(name);
                inserted_name = true;
            }
            else
            {
                name_index = static_cast<std::uint32_t>(
                    iterator - names_->begin()
                );
            }

            appendLittle(*destination_, name_index);
            destination_->push_back(static_cast<std::byte>(type));
            appendLittle(
                *destination_, static_cast<std::uint32_t>(bytes.size())
            );
            last_payload_offset_ = static_cast<std::uint32_t>(
                destination_->size()
            );
            destination_->insert(
                destination_->end(), bytes.begin(), bytes.end()
            );
            ++property_count_;
            return {};
        }
        catch (...)
        {
            destination_->resize(destination_size);
            if (inserted_name)
                names_->pop_back();
            return lux::cxx::unexpected(
                EComponentCodecError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<void, EComponentCodecError>
    TaggedPropertyWriter::finish() noexcept
    {
        if (allocation_failed_)
        {
            return lux::cxx::unexpected(
                EComponentCodecError::ALLOCATION_FAILURE
            );
        }
        if (finished_)
            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
        if (destination_->size() < sizeof(std::uint32_t))
        {
            return lux::cxx::unexpected(
                EComponentCodecError::ALLOCATION_FAILURE
            );
        }
        patchU32(*destination_, 0, property_count_);
        finished_ = true;
        return {};
    }

    TaggedPropertyReader::TaggedPropertyReader(
        std::span<const std::byte> bytes,
        std::span<const std::string> name_table,
        TaggedPropertyLimits limits
    ) noexcept
        : bytes_(bytes), names_(name_table), limits_(limits)
    {
        if (!readLittle(bytes_, offset_, remaining_) ||
            remaining_ > limits_.max_properties)
        {
            valid_ = false;
        }
    }

    bool TaggedPropertyReader::next(EncodedPropertyView& property) noexcept
    {
        if (!valid_ || remaining_ == 0)
            return false;

        std::uint32_t name{};
        std::uint8_t type{};
        std::uint32_t size{};
        if (!readLittle(bytes_, offset_, name) ||
            !readLittle(bytes_, offset_, type) ||
            !readLittle(bytes_, offset_, size) ||
            name >= names_.size() ||
            type > static_cast<std::uint8_t>(
                EComponentWireType::STABLE_REFERENCE
            ) ||
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
