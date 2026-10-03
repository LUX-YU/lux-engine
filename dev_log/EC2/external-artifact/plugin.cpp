#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <string>
namespace p = lux::editor::persistence;
class Source final : public p::IArtifactSource
{
public:
    explicit Source(std::string value) : value_(std::move(value)) {}
    p::PersistenceResult<p::EncodedArtifact> encode(std::stop_token stop) const override
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(p::PersistenceFailure{p::EPersistenceError::CANCELLED});
        auto bytes = std::make_shared<const std::string>(value_);
        return p::EncodedArtifact{lux::cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)))};
    }
private:
    std::string value_;
};
extern "C" __declspec(dllexport) p::IArtifactSource* makeSource(const char* bytes, std::size_t size)
{
    return new Source{std::string(bytes, size)};
}
