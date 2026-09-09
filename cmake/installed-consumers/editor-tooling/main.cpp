#include "Tool.hpp"
#include <cassert>
#include <cstdio>
#include <string_view>
int main(int argc, char **argv)
{
    using namespace lux::editor::application;
    Counts counts;
    {
        Toolset tools;
        auto installed = installTool(tools, counts);
        assert(installed && tools.find<ConsumerTool>() == &installed->get());
        const auto duplicate = tools.install<ConsumerTool>(counts);
        assert(!duplicate && duplicate.error().code == EToolsetError::DUPLICATE_TOOL);
        tools.freeze();
        const auto frozen = installTool(tools, counts);
        assert(!frozen && frozen.error().code == EToolsetError::FROZEN);
        tools.requestStop();
        tools.requestStop();
        assert(counts.constructed == 1 && counts.stopped == 1 && counts.destroyed == 0);
    }
    assert(counts.constructed == 1 && counts.stopped == 1 && counts.destroyed == 1);
    std::puts("installed Toolset PASS typed DLL install/lookup/freeze/stop/destruction");
    if (argc == 2 && std::string_view{argv[1]} == "--hold-for-module-audit")
    {
        std::fflush(stdout);
        static_cast<void>(std::getchar());
    }
}
