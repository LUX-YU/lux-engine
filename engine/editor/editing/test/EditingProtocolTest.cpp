#include "CoreCases.hpp"
#include "FailureCases.hpp"
#include "SaveLimitCases.hpp"

int main()
{
    using namespace lux::editor::editing::test;
    coreCases();
    failureCases();
    saveLimitCases();
}
