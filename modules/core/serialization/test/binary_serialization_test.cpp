#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    enum class EMode : std::uint16_t
    {
        FIRST = 1,
        SECOND = 0x0203,
    };

    struct Record final
    {
        std::uint32_t id{};
        bool enabled{};
        EMode mode{};
        std::string name;
        std::vector<std::int16_t> values;
        std::optional<float> weight;

        friend bool operator==(const Record&, const Record&) = default;
    };

    struct ThrowingCustom final {};
    struct AllocatingCustom final {};
    struct EmptyCustom final {};
}

template <>
struct lux::serialization::Serializer<ThrowingCustom>
{
    template <class Writer>
    static SerializationResult write(
        Writer&,
        const ThrowingCustom&,
        const SerializationContext&
    )
    {
        throw std::runtime_error("custom serializer failure");
    }
};

template <>
struct lux::serialization::Serializer<AllocatingCustom>
{
    template <class Writer>
    static SerializationResult write(
        Writer&,
        const AllocatingCustom&,
        const SerializationContext&
    )
    {
        throw std::bad_alloc{};
    }
};

template <>
struct lux::serialization::Serializer<EmptyCustom>
{
    static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
    static constexpr std::size_t fixed_wire_size = 4U;

    template <class Writer>
    static SerializationResult write(
        Writer& writer,
        const EmptyCustom&,
        const SerializationContext& context
    )
    {
        return lux::serialization::write(
            writer,
            std::uint32_t{},
            context
        );
    }

    template <class Reader>
    static SerializationResult read(
        Reader& reader,
        EmptyCustom&,
        const SerializationContext& context
    )
    {
        std::uint32_t ignored{};
        return lux::serialization::read(reader, ignored, context);
    }
};

template <>
struct lux::meta::TypeStaticInfo<Record>
{
    static constexpr bool available = true;
    static constexpr auto fields = std::make_tuple(
        typeStaticField<&Record::id>("id"),
        typeStaticField<&Record::enabled>("enabled"),
        typeStaticField<&Record::mode>("mode"),
        typeStaticField<&Record::name>("name"),
        typeStaticField<&Record::values>("values"),
        typeStaticField<&Record::weight>("weight")
    );
};

int main()
{
    using namespace lux::serialization;
    static_assert(WireTraits<EmptyCustom>::extent == EWireExtent::FIXED);
    static_assert(WireTraits<EmptyCustom>::fixed_size == 4U);
    assert(binarySerializationContractVersion() == 1U);
    const SerializationBudget budget(1024U, 1024U, 16U);

    Record source{
        0x11223344U,
        true,
        EMode::SECOND,
        "portable",
        {-3, 4, 1024},
        1.25F
    };
    std::vector<std::byte> bytes;
    BinaryWriter writer(bytes);
    assert(write(writer, source, budget));

    assert(bytes.size() > 7U);
    assert(bytes[0] == std::byte{0x44});
    assert(bytes[1] == std::byte{0x33});
    assert(bytes[2] == std::byte{0x22});
    assert(bytes[3] == std::byte{0x11});
    assert(bytes[4] == std::byte{0x01});
    assert(bytes[5] == std::byte{0x03});
    assert(bytes[6] == std::byte{0x02});

    BinaryReader reader(bytes);
    auto decoded = read<Record>(reader, budget);
    assert(decoded);
    assert(*decoded == source);
    assert(reader.remaining() == 0U);

    auto invalid_bool = bytes;
    invalid_bool[4] = std::byte{0x02};
    BinaryReader invalid_reader(invalid_bool);
    Record invalid{};
    auto invalid_result = read(invalid_reader, invalid, budget);
    assert(!invalid_result);
    assert(invalid_result.error().code == ESerializationError::INVALID_VALUE);

    BinaryReader truncated(std::span<const std::byte>(bytes).first(6U));
    Record partial{};
    auto truncated_result = read(truncated, partial, budget);
    assert(!truncated_result);
    assert(truncated_result.error().code == ESerializationError::TRUNCATED);

    Eigen::Matrix<float, 2, 2, Eigen::RowMajor> matrix;
    matrix << 1.0F, 2.0F, 3.0F, 4.0F;
    std::vector<std::byte> matrix_bytes;
    BinaryWriter matrix_writer(matrix_bytes);
    assert(write(matrix_writer, matrix, budget));
    BinaryReader matrix_reader(matrix_bytes);
    auto decoded_matrix = read<decltype(matrix)>(matrix_reader, budget);
    assert(decoded_matrix);
    assert(decoded_matrix->isApprox(matrix));

    const Eigen::Quaternionf quaternion(4.0F, 1.0F, 2.0F, 3.0F);
    std::vector<std::byte> quaternion_bytes;
    BinaryWriter quaternion_writer(quaternion_bytes);
    assert(write(quaternion_writer, quaternion, budget));
    BinaryReader quaternion_reader(quaternion_bytes);
    auto decoded_quaternion = read<Eigen::Quaternionf>(
        quaternion_reader,
        budget
    );
    assert(decoded_quaternion);
    assert(decoded_quaternion->coeffs().isApprox(
        quaternion.normalized().coeffs()
    ));

    std::vector<std::byte> failure_bytes;
    BinaryWriter failure_writer(failure_bytes);
    auto throwing_custom = write(
        failure_writer,
        ThrowingCustom{},
        budget
    );
    assert(!throwing_custom);
    assert(throwing_custom.error().code == ESerializationError::INVALID_VALUE);
    auto allocating_custom = write(
        failure_writer,
        AllocatingCustom{},
        budget
    );
    assert(!allocating_custom);
    assert(allocating_custom.error().code ==
        ESerializationError::ALLOCATION_FAILURE);

    return 0;
}
