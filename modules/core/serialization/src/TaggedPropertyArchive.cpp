#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <lux/cxx/compile_time/type_info.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <uuid.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace lux::serialize
{
    namespace
    {
        // ─────────────────────────────────────────────────────────────────
        //  Type classification — RefField → EArchiveType
        // ─────────────────────────────────────────────────────────────────

        /// Hash-keyed dispatch for Record-typed leaves (Eigen, std::string,
        /// asset_id_t, std::array<float, 8>). Constructed once on first
        /// use; safe-static for thread-safe lazy init.
        const std::unordered_map<std::uint64_t, EArchiveType>& recordHashToType()
        {
            static const std::unordered_map<std::uint64_t, EArchiveType> kTable = {
                { lux::cxx::type_hash<std::string>(),            EArchiveType::String       },
                { lux::cxx::type_hash<std::string_view>(),       EArchiveType::String       },
                { lux::cxx::type_hash<Eigen::Vector2f>(),        EArchiveType::Vec2f        },
                { lux::cxx::type_hash<Eigen::Vector3f>(),        EArchiveType::Vec3f        },
                { lux::cxx::type_hash<Eigen::Vector4f>(),        EArchiveType::Vec4f        },
                { lux::cxx::type_hash<Eigen::Quaternionf>(),     EArchiveType::Quatf        },
                { lux::cxx::type_hash<Eigen::Matrix4f>(),        EArchiveType::Mat4f        },
                { lux::cxx::type_hash<std::array<float, 8>>(),   EArchiveType::ArrayFloat8  },
                { lux::cxx::type_hash<uuids::uuid>(),            EArchiveType::AssetRef     },
            };
            return kTable;
        }

        /// Classify a field's storage type into an `EArchiveType`. Returns
        /// `Skip` if the type is not yet supported — caller currently aborts
        /// the write (failing loudly during dev) rather than silently
        /// emitting a zero-sized payload.
        EArchiveType classifyField(const lux::meta::RefField& field)
        {
            const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);

            // Primitives dispatch on the base enum.
            switch (base)
            {
                case lux::meta::EBaseType::Bool:   return EArchiveType::Bool;
                case lux::meta::EBaseType::Int32:  return EArchiveType::Int32;
                case lux::meta::EBaseType::Uint32: return EArchiveType::UInt32;
                case lux::meta::EBaseType::Int64:  return EArchiveType::Int64;
                case lux::meta::EBaseType::Uint64: return EArchiveType::UInt64;
                case lux::meta::EBaseType::Float:  return EArchiveType::Float;
                case lux::meta::EBaseType::Double: return EArchiveType::Double;

                // Narrow integers are widened on serialize so the on-disk
                // form stays canonical regardless of how the source field
                // was declared. `Int8`/`UInt8` etc. all read/write as i32.
                case lux::meta::EBaseType::Int8:
                case lux::meta::EBaseType::Int16:
                    return EArchiveType::Int32;
                case lux::meta::EBaseType::Uint8:
                case lux::meta::EBaseType::Uint16:
                    return EArchiveType::UInt32;

                default:
                    break;
            }

            if (base != lux::meta::EBaseType::Record)
                return EArchiveType::Skip;

            // Record: try the leaf table first.
            const auto& table = recordHashToType();
            if (auto it = table.find(field.type.hash); it != table.end())
                return it->second;

            // Unknown reflected struct: only recursable if the type carries
            // its own RefClass back-pointer. (Set by the meta generator for
            // every `LUX_CLASS`/`LUX_COMPONENT` reflected type.)
            if (field.type.ptr != nullptr)
                return EArchiveType::Struct;

            return EArchiveType::Skip;
        }

        [[nodiscard]] const void* fieldPtr(const lux::meta::RefField& f,
                                           const void* obj) noexcept
        {
            return static_cast<const std::byte*>(obj) + f.offset;
        }
        [[nodiscard]] void* fieldPtr(const lux::meta::RefField& f, void* obj) noexcept
        {
            return static_cast<std::byte*>(obj) + f.offset;
        }

        [[nodiscard]] bool isExcludedField(
            const lux::meta::RefField& field) noexcept
        {
            if (field.visibility != lux::meta::EVisibility::Public)
                return true;
            if (field.annotation_str)
            {
                const std::string_view annotations{field.annotation_str};
                if (annotations.find("luxref::property::skip") !=
                        std::string_view::npos ||
                    field.annotations().has("cooked_relocation"))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isSerializedField(
            const lux::meta::RefField& field) noexcept
        {
            return !isExcludedField(field) &&
                classifyField(field) != EArchiveType::Skip;
        }

        // ─────────────────────────────────────────────────────────────────
        //  Leaf writers (one per EArchiveType that needs custom layout).
        //  Each writes the payload only — header is written by the caller.
        //  Returns nothing; ArchiveWriter advance is implicit.
        // ─────────────────────────────────────────────────────────────────

        void writeLeafPayload(EArchiveType type_id,
                              const lux::meta::RefField& field,
                              const void* field_ptr,
                              ArchiveWriter& ar)
        {
            switch (type_id)
            {
                case EArchiveType::Bool:
                {
                    const bool v = *static_cast<const bool*>(field_ptr);
                    const std::uint8_t b = v ? 1u : 0u;
                    ar.writePod(b);
                    return;
                }
                case EArchiveType::Int32:
                {
                    // Narrow ints get widened to i32 on the wire.
                    std::int32_t v = 0;
                    const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);
                    if (base == lux::meta::EBaseType::Int8)
                        v = *static_cast<const std::int8_t*>(field_ptr);
                    else if (base == lux::meta::EBaseType::Int16)
                        v = *static_cast<const std::int16_t*>(field_ptr);
                    else
                        v = *static_cast<const std::int32_t*>(field_ptr);
                    ar.writePod(v);
                    return;
                }
                case EArchiveType::UInt32:
                {
                    std::uint32_t v = 0;
                    const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);
                    if (base == lux::meta::EBaseType::Uint8)
                        v = *static_cast<const std::uint8_t*>(field_ptr);
                    else if (base == lux::meta::EBaseType::Uint16)
                        v = *static_cast<const std::uint16_t*>(field_ptr);
                    else
                        v = *static_cast<const std::uint32_t*>(field_ptr);
                    ar.writePod(v);
                    return;
                }
                case EArchiveType::Int64:
                    ar.writePod(*static_cast<const std::int64_t*>(field_ptr));
                    return;
                case EArchiveType::UInt64:
                    ar.writePod(*static_cast<const std::uint64_t*>(field_ptr));
                    return;
                case EArchiveType::Float:
                    ar.writePod(*static_cast<const float*>(field_ptr));
                    return;
                case EArchiveType::Double:
                    ar.writePod(*static_cast<const double*>(field_ptr));
                    return;
                case EArchiveType::String:
                    ar.writeString(*static_cast<const std::string*>(field_ptr));
                    return;
                case EArchiveType::Vec2f:
                {
                    const auto& v = *static_cast<const Eigen::Vector2f*>(field_ptr);
                    ar.writeBytes(v.data(), 2 * sizeof(float));
                    return;
                }
                case EArchiveType::Vec3f:
                {
                    const auto& v = *static_cast<const Eigen::Vector3f*>(field_ptr);
                    ar.writeBytes(v.data(), 3 * sizeof(float));
                    return;
                }
                case EArchiveType::Vec4f:
                {
                    const auto& v = *static_cast<const Eigen::Vector4f*>(field_ptr);
                    ar.writeBytes(v.data(), 4 * sizeof(float));
                    return;
                }
                case EArchiveType::Quatf:
                {
                    const auto& q = *static_cast<const Eigen::Quaternionf*>(field_ptr);
                    const float xyzw[4] = { q.x(), q.y(), q.z(), q.w() };
                    ar.writeBytes(xyzw, sizeof(xyzw));
                    return;
                }
                case EArchiveType::Mat4f:
                {
                    const auto& m = *static_cast<const Eigen::Matrix4f*>(field_ptr);
                    ar.writeBytes(m.data(), 16 * sizeof(float));
                    return;
                }
                case EArchiveType::ArrayFloat8:
                {
                    const auto& a = *static_cast<const std::array<float, 8>*>(field_ptr);
                    ar.writeBytes(a.data(), 8 * sizeof(float));
                    return;
                }
                case EArchiveType::AssetRef:
                    ar.writeUuid(*static_cast<const uuids::uuid*>(field_ptr));
                    return;
                case EArchiveType::Struct:
                case EArchiveType::Skip:
                    // Handled in writeField with its own logic.
                    return;
            }
        }

        void readLeafPayload(EArchiveType type_id,
                              const lux::meta::RefField& field,
                              void* field_ptr,
                              std::uint32_t payload_size,
                              ArchiveReader& ar,
                              bool require_exact_size = false)
        {
            // The `payload_size` parameter lets us cope with a future change
            // where the on-disk leaf grew — we always consume exactly the
            // declared size, even if we deserialize less.
            const std::size_t start = ar.tell();

            auto skip_remainder = [&]() {
                const std::size_t consumed = ar.tell() - start;
                if (consumed < payload_size)
                    ar.skip(payload_size - consumed);
            };

            switch (type_id)
            {
                case EArchiveType::Bool:
                {
                    const auto b = ar.readPod<std::uint8_t>();
                    if (require_exact_size && b > 1u)
                        ar.invalidate();
                    else
                        *static_cast<bool*>(field_ptr) = (b != 0u);
                    break;
                }
                case EArchiveType::Int32:
                {
                    const auto v = ar.readPod<std::int32_t>();
                    const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);
                    if (base == lux::meta::EBaseType::Int8)
                        *static_cast<std::int8_t*>(field_ptr) = static_cast<std::int8_t>(v);
                    else if (base == lux::meta::EBaseType::Int16)
                        *static_cast<std::int16_t*>(field_ptr) = static_cast<std::int16_t>(v);
                    else
                        *static_cast<std::int32_t*>(field_ptr) = v;
                    break;
                }
                case EArchiveType::UInt32:
                {
                    const auto v = ar.readPod<std::uint32_t>();
                    const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);
                    if (base == lux::meta::EBaseType::Uint8)
                        *static_cast<std::uint8_t*>(field_ptr) = static_cast<std::uint8_t>(v);
                    else if (base == lux::meta::EBaseType::Uint16)
                        *static_cast<std::uint16_t*>(field_ptr) = static_cast<std::uint16_t>(v);
                    else
                        *static_cast<std::uint32_t*>(field_ptr) = v;
                    break;
                }
                case EArchiveType::Int64:
                    *static_cast<std::int64_t*>(field_ptr) = ar.readPod<std::int64_t>();
                    break;
                case EArchiveType::UInt64:
                    *static_cast<std::uint64_t*>(field_ptr) = ar.readPod<std::uint64_t>();
                    break;
                case EArchiveType::Float:
                {
                    const auto value = ar.readPod<float>();
                    if (!std::isfinite(value))
                        ar.invalidate();
                    else
                        *static_cast<float*>(field_ptr) = value;
                    break;
                }
                case EArchiveType::Double:
                {
                    const auto value = ar.readPod<double>();
                    if (!std::isfinite(value))
                        ar.invalidate();
                    else
                        *static_cast<double*>(field_ptr) = value;
                    break;
                }
                case EArchiveType::String:
                    *static_cast<std::string*>(field_ptr) = ar.readString();
                    break;
                case EArchiveType::Vec2f:
                {
                    auto& v = *static_cast<Eigen::Vector2f*>(field_ptr);
                    ar.readBytes(v.data(), 2 * sizeof(float));
                    break;
                }
                case EArchiveType::Vec3f:
                {
                    auto& v = *static_cast<Eigen::Vector3f*>(field_ptr);
                    ar.readBytes(v.data(), 3 * sizeof(float));
                    break;
                }
                case EArchiveType::Vec4f:
                {
                    auto& v = *static_cast<Eigen::Vector4f*>(field_ptr);
                    ar.readBytes(v.data(), 4 * sizeof(float));
                    break;
                }
                case EArchiveType::Quatf:
                {
                    float xyzw[4];
                    ar.readBytes(xyzw, sizeof(xyzw));
                    auto& q = *static_cast<Eigen::Quaternionf*>(field_ptr);
                    q.x() = xyzw[0]; q.y() = xyzw[1]; q.z() = xyzw[2]; q.w() = xyzw[3];
                    break;
                }
                case EArchiveType::Mat4f:
                {
                    auto& m = *static_cast<Eigen::Matrix4f*>(field_ptr);
                    ar.readBytes(m.data(), 16 * sizeof(float));
                    break;
                }
                case EArchiveType::ArrayFloat8:
                {
                    auto& a = *static_cast<std::array<float, 8>*>(field_ptr);
                    ar.readBytes(a.data(), 8 * sizeof(float));
                    break;
                }
                case EArchiveType::AssetRef:
                    *static_cast<uuids::uuid*>(field_ptr) = ar.readUuid();
                    break;
                case EArchiveType::Struct:
                case EArchiveType::Skip:
                    // Recurse / skip handled in readObject.
                    ar.skip(payload_size);
                    return;
            }

            const auto consumed = ar.tell() - start;
            if (require_exact_size && consumed != payload_size)
                ar.invalidate();
            else
                skip_remainder();
        }
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────
    //  Writer
    // ─────────────────────────────────────────────────────────────────

    void TaggedPropertyWriter::writeObject(const lux::meta::RefClass& cls,
                                             const void* obj)
    {
        for (const auto& field : cls.fields)
        {
            // `cooked_relocation` fields are reflected so EntityScene can
            // validate their exact destination type and offset, but their
            // value lives exclusively in an LXES relocation table.  They are
            // no more part of the generic tagged-property payload than an
            // explicitly skipped runtime field.
            if (isExcludedField(field))
                continue;

            writeField(field, obj);
        }
        ar_->writePod(kEndOfObject);
    }

    void TaggedPropertyWriter::writeField(const lux::meta::RefField& field,
                                           const void* obj)
    {
        const EArchiveType type_id = classifyField(field);
        if (type_id == EArchiveType::Skip)
        {
            std::fprintf(stderr,
                "[serialize] field '%.*s' has no archive leaf type "
                "(type=%.*s); not serialized.\n",
                static_cast<int>(field.name.size()), field.name.data(),
                static_cast<int>(field.type.name.size()), field.type.name.data());
            return;
        }

        // Header: name_idx, type_id, reserve size.
        const std::uint32_t name_idx = names_->intern(field.name);
        ar_->writePod(name_idx);
        ar_->writePod(static_cast<std::uint8_t>(type_id));
        const std::size_t size_off = ar_->reserveU32();
        const std::size_t payload_start = ar_->tell();

        const void* fp = fieldPtr(field, obj);
        if (type_id == EArchiveType::Struct)
        {
            // Nested reflected struct — recurse with a fresh writer that
            // shares our NameTable. Payload is itself a tagged-property
            // block terminated by kEndOfObject.
            const auto* nested_cls = static_cast<const lux::meta::RefClass*>(field.type.ptr);
            assert(nested_cls && "Struct field with null RefClass pointer");
            TaggedPropertyWriter nested(*ar_, *names_);
            nested.writeObject(*nested_cls, fp);
        }
        else
        {
            writeLeafPayload(type_id, field, fp, *ar_);
        }

        const std::size_t payload_end = ar_->tell();
        ar_->patchU32At(size_off, static_cast<std::uint32_t>(payload_end - payload_start));
    }

    // ─────────────────────────────────────────────────────────────────
    //  Reader
    // ─────────────────────────────────────────────────────────────────

    void TaggedPropertyReader::readObject(const lux::meta::RefClass& cls,
                                           void* obj)
    {
        while (true)
        {
            const auto name_idx = ar_->readPod<std::uint32_t>();
            if (!ar_->ok())
                return;
            if (name_idx == kEndOfObject)
                return;

            const auto wire_type    = static_cast<EArchiveType>(ar_->readPod<std::uint8_t>());
            const auto payload_size = ar_->readPod<std::uint32_t>();
            if (!ar_->ok())
                return;

            const std::string_view name = names_->at(name_idx);

            // Locate matching field by name in the current RefClass.
            const lux::meta::RefField* match = nullptr;
            for (const auto& f : cls.fields)
            {
                if (f.name == name) { match = &f; break; }
            }

            if (match == nullptr)
            {
                // Unknown field — schema drift / older code. Skip and move on.
                ar_->skip(payload_size);
                continue;
            }

            const EArchiveType expected = classifyField(*match);

            if (wire_type == EArchiveType::Struct &&
                expected == EArchiveType::Struct)
            {
                const auto* nested_cls = static_cast<const lux::meta::RefClass*>(match->type.ptr);
                if (!nested_cls)
                {
                    ar_->skip(payload_size);
                    continue;
                }
                TaggedPropertyReader nested(*ar_, *names_);
                // Bound the nested read to the declared payload size by
                // checking after — the EndOfObject marker inside the nested
                // block is what truly terminates it; payload_size is the
                // skip-safety bound for unknown nested types.
                nested.readObject(*nested_cls, fieldPtr(*match, obj));
                continue;
            }

            if (wire_type != expected)
            {
                // Type drift (e.g. float field renamed to double, or
                // changed kind). Skip rather than corrupt memory.
                ar_->skip(payload_size);
                continue;
            }

            readLeafPayload(wire_type, *match, fieldPtr(*match, obj),
                            payload_size, *ar_);
        }
    }

    bool TaggedPropertyReader::readObjectExact(
        const lux::meta::RefClass& cls,
        void* obj)
    {
        for (const auto& field : cls.fields)
        {
            if (!isSerializedField(field))
                continue;

            const auto name_index = ar_->readPod<std::uint32_t>();
            if (!ar_->ok() || name_index == kEndOfObject ||
                name_index >= names_->size() ||
                names_->at(name_index) != field.name)
            {
                ar_->invalidate();
                return false;
            }

            const auto wire_type = static_cast<EArchiveType>(
                ar_->readPod<std::uint8_t>());
            const auto payload_size = ar_->readPod<std::uint32_t>();
            const auto expected_type = classifyField(field);
            if (!ar_->ok() || wire_type != expected_type ||
                payload_size > ar_->remaining())
            {
                ar_->invalidate();
                return false;
            }

            const auto payload_start = ar_->tell();
            if (wire_type == EArchiveType::Struct)
            {
                const auto* nested_class =
                    static_cast<const lux::meta::RefClass*>(field.type.ptr);
                if (!nested_class)
                {
                    ar_->invalidate();
                    return false;
                }
                TaggedPropertyReader nested{*ar_, *names_};
                if (!nested.readObjectExact(
                        *nested_class,
                        fieldPtr(field, obj)))
                {
                    ar_->invalidate();
                    return false;
                }
            }
            else
            {
                readLeafPayload(
                    wire_type,
                    field,
                    fieldPtr(field, obj),
                    payload_size,
                    *ar_,
                    true);
            }

            if (!ar_->ok() || ar_->tell() - payload_start != payload_size)
            {
                ar_->invalidate();
                return false;
            }
        }

        const auto end = ar_->readPod<std::uint32_t>();
        if (!ar_->ok() || end != kEndOfObject)
        {
            ar_->invalidate();
            return false;
        }
        return true;
    }

    bool isAssetRefField(const lux::meta::RefField& field)
    {
        return classifyField(field) == EArchiveType::AssetRef;
    }

} // namespace lux::serialize
