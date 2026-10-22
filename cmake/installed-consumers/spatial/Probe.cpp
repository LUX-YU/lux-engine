#include <lux/engine/render/runtime/FeatureRegistration.hpp>
#include <probe/Probe.hpp>

namespace probe
{
    class ProbeCommandHandler final
    {
    public:
        static ProbeCommandHandler* create(lux::render::ResourceRegistry&) noexcept
        {
            return new ProbeCommandHandler;
        }

        static constexpr std::string_view name = "test.external.ProbeCommandHandler";

        void set(const SetValues::Request& input, SetValues::Response& output) noexcept
        {
            output = {};
            for (const auto& value : input.values)
            {
                output.sequence = value.sequence;
                output.sum += value.amount;
            }
        }

        using Bindings = std::tuple<lux::render::CommandMethod<SetValues, &ProbeCommandHandler::set>>;
    };

    lux::render::RenderResult<lux::render::HandlerId> install(lux::render::RenderServer& server) noexcept
    {
        return server.registerHandler<ProbeCommandHandler>("external");
    }
} // namespace probe
