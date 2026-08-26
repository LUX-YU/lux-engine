#pragma once

#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/Traits.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <cmath>
#include <limits>

namespace lux::serialization
{
    template <class Scalar, int Rows, int Columns, int Options, int MaxRows, int MaxColumns>
    struct Serializer<Eigen::Matrix<Scalar, Rows, Columns, Options, MaxRows, MaxColumns>>
    {
        using Matrix = Eigen::Matrix<Scalar, Rows, Columns, Options, MaxRows, MaxColumns>;
        static constexpr EWireExtent wire_extent =
            Rows == Eigen::Dynamic || Columns == Eigen::Dynamic ||
                WireTraits<Scalar>::extent == EWireExtent::VARIABLE
            ? EWireExtent::VARIABLE
            : WireTraits<Scalar>::extent;
        static constexpr std::size_t fixed_wire_size =
            wire_extent != EWireExtent::FIXED
            ? 0U
            : WireTraits<Scalar>::fixed_size * static_cast<std::size_t>(Rows) *
                static_cast<std::size_t>(Columns);

        template <class Writer>
        [[nodiscard]] static SerializationResult write(
            Writer& writer,
            const Matrix& value
        ) noexcept
        {
            SerializationResult result{};
            if constexpr (Rows == Eigen::Dynamic)
            {
                result = writer.template writeUnsigned<std::uint64_t>(value.rows());
            }
            if constexpr (Columns == Eigen::Dynamic)
            {
                if (result)
                {
                    result = writer.template writeUnsigned<std::uint64_t>(value.cols());
                }
            }
            for (Eigen::Index row{}; result && row < value.rows(); ++row)
            {
                for (Eigen::Index column{}; result && column < value.cols(); ++column)
                {
                    result = serialization::write(writer, value(row, column));
                }
            }
            return result;
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            Matrix& value
        ) noexcept
        {
            std::uint64_t rows = Rows == Eigen::Dynamic ? 0U : static_cast<std::uint64_t>(Rows);
            std::uint64_t columns = Columns == Eigen::Dynamic
                ? 0U
                : static_cast<std::uint64_t>(Columns);
            if constexpr (Rows == Eigen::Dynamic)
            {
                auto encoded = reader.template readUnsigned<std::uint64_t>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                rows = *encoded;
            }
            if constexpr (Columns == Eigen::Dynamic)
            {
                auto encoded = reader.template readUnsigned<std::uint64_t>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                columns = *encoded;
            }
            if (rows > reader.limits().max_container_elements ||
                columns > reader.limits().max_container_elements ||
                (columns != 0U && rows > reader.limits().max_container_elements / columns))
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::LIMIT_EXCEEDED,
                    reader.offset()
                });
            }
            try
            {
                value.resize(
                    static_cast<Eigen::Index>(rows),
                    static_cast<Eigen::Index>(columns)
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::ALLOCATION_FAILURE,
                    reader.offset()
                });
            }
            SerializationResult result{};
            for (Eigen::Index row{}; result && row < value.rows(); ++row)
            {
                for (Eigen::Index column{}; result && column < value.cols(); ++column)
                {
                    result = serialization::read(reader, value(row, column));
                }
            }
            return result;
        }
    };

    template <class Scalar, int Options>
    struct Serializer<Eigen::Quaternion<Scalar, Options>>
    {
        using Quaternion = Eigen::Quaternion<Scalar, Options>;
        static constexpr EWireExtent wire_extent =
            WireTraits<Scalar>::extent;
        static constexpr std::size_t fixed_wire_size =
            wire_extent == EWireExtent::FIXED
            ? WireTraits<Scalar>::fixed_size * 4U
            : 0U;

        template <class Writer>
        [[nodiscard]] static SerializationResult write(
            Writer& writer,
            const Quaternion& value
        ) noexcept
        {
            SerializationResult result = serialization::write(writer, value.x());
            if (result) result = serialization::write(writer, value.y());
            if (result) result = serialization::write(writer, value.z());
            if (result) result = serialization::write(writer, value.w());
            return result;
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            Quaternion& value
        ) noexcept
        {
            Scalar x{};
            Scalar y{};
            Scalar z{};
            Scalar w{};
            SerializationResult result = serialization::read(reader, x);
            if (result) result = serialization::read(reader, y);
            if (result) result = serialization::read(reader, z);
            if (result) result = serialization::read(reader, w);
            if (result)
            {
                value = Quaternion(w, x, y, z);
                if (!std::isfinite(value.squaredNorm()) ||
                    value.squaredNorm() <= std::numeric_limits<Scalar>::epsilon())
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::INVALID_VALUE,
                        reader.offset()
                    });
                }
                value.normalize();
            }
            return result;
        }
    };
} // namespace lux::serialization
