#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <new>

namespace lux::editor::persistence
{
    namespace
    {
        cxx::SharedBytes<> pinBytes(const lux::object::CodeLease& code, cxx::SharedBytes<> bytes)
        {
            if (code.sameOwner(lux::object::CodeLease::builtin()) || bytes.empty())
                return bytes;
            struct Owner final
            {
                lux::object::CodeLease code;
                cxx::SharedBytes<> bytes;
            };
            auto owner = std::make_shared<const Owner>(code, std::move(bytes));
            return cxx::SharedBytes<>::fromOwner(owner, owner->bytes.view());
        }
    }
    DerivedArtifact::DerivedArtifact(
        lux::object::CodeLease code,
        DerivedArtifactInfo info,
        cxx::SharedBytes<> bytes,
        std::shared_ptr<const IArtifactSource> source
    )
        : code_(std::move(code)), info_(std::move(info)), bytes_(pinBytes(code_, std::move(bytes))),
          source_(lux::object::pinCodeOwner(code_, std::move(source)))
    {}
    DerivedArtifact::DerivedArtifact(const DerivedArtifact& other)
        : code_(other.code_), info_(other.info_), bytes_(other.bytes_),
          source_(lux::object::pinCodeOwner(code_, other.source_))
    {}
    DerivedArtifact& DerivedArtifact::operator=(DerivedArtifact other) noexcept
    {
        using std::swap;
        swap(code_, other.code_);
        swap(info_, other.info_);
        swap(bytes_, other.bytes_);
        swap(source_, other.source_);
        return *this;
    }
    bool DerivedArtifact::valid() const noexcept
    {
        const bool has_source =
            code_.valid() && source_ && info_.content.session.valid() && !info_.source_asset.isNull();
        const bool has_format = !info_.canonical_type.empty() && info_.encoding_version && info_.primary_magic;
        return has_source && has_format && !bytes_.empty();
    }
    PersistenceResult<EncodedArtifact> DerivedArtifact::encodeSource(std::stop_token stop) const noexcept
    {
        if (!valid())
            return cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        if (stop.stop_requested())
            return cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        if (code_.sameOwner(lux::object::CodeLease::builtin()))
            return source_->encode(stop);
        try // Only the foreign encoder boundary contains exceptions.
        {
            auto encoded = source_->encode(stop);
            if (encoded)
                encoded->bytes = pinBytes(code_, std::move(encoded->bytes));
            return encoded;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE, "Foreign source encoder exception"});
        }
    }
}
