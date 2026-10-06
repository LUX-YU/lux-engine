#include <lux/engine/editor/persistence/EncodeJob.hpp>
#include <new>

namespace lux::editor::persistence
{
    // Explicit plugin/foreign encoder boundary. Lux semantic failures stay in the typed channel.
    PersistenceResult<EncodedArtifact> OwnedEncodeJob::encode(std::stop_token stop) noexcept
    try
    {
        if (!code.valid() || !job)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        if (stop.stop_requested())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        return job->encode(stop);
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE, "Foreign encoder exception"});
    }
}
