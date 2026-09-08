#include "EditingFixtures.hpp"
#include <cstdio>
#include <cstdlib>
#include <lux/engine/editor/editing/detail/EditDiagnostics.hpp>
#include <string_view>
#include <thread>
using namespace lux::editor::editing;
using namespace lux::editor::editing::test;
namespace
{
    void expectedContract(detail::EEditContract reason) noexcept
    {
        if (reason == detail::EEditContract::HISTORY_LIFETIME)
        {
            std::fputs("ED1_CONTRACT_HISTORY_LIFETIME\n", stderr);
            std::_Exit(73);
        }
        std::_Exit(74);
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string_view mode(argv[1]);
    if (mode == "wrong-thread")
    {
        auto history = EditHistory::create({kLimits});
        assert(history);
        auto* raw = history->release();
        std::thread foreign(
            [raw]
            {
                detail::editDiagnostics().contract = expectedContract;
                delete raw;
            }
        );
        foreign.join();
    }
    else if (mode == "busy")
    {
        auto session = std::make_unique<TextSession>();
        detail::editDiagnostics().contract = expectedContract;
        session->callback = [&](EStage stage)
        {
            if (stage == EStage::PUBLISH)
            {
                delete session->history.release();
            }
        };
        execute(*session, session->replace(0U, "alpha", "beta"));
    }
    return 1;
}
