#include "Consumer.hpp"
#include <filesystem>
#include <iostream>
#if defined(_WIN32)
#include <Windows.h>
#endif
int main(int argc, char** argv)
{
#if defined(_WIN32)
    assert(argc == 2);
    char path[32768]{};
    const auto module = GetModuleHandleA("lux_engine_editor_editing.dll");
    assert(module && GetModuleFileNameA(module, path, sizeof(path)));
    assert(std::filesystem::equivalent(path, std::filesystem::path(argv[1]) / "lux_engine_editor_editing.dll"));
    std::cout << "loaded_core=" << path << '\n';
#endif
    const auto text = runTextConsumer();
    const auto records = runRecordsConsumer();
    const auto again = runTextConsumer();
    assert(text.identity.valid() && records.identity.valid() && again.identity.valid());
    assert(text.identity != records.identity && again.identity != text.identity && again.identity != records.identity);
    assert(text.operations == 1U && records.operations == 1U && text.plans == 3U && records.plans == 3U);
    assert(text.checksum == 4U && records.checksum == 42U);
    std::cout << "C02 PASS ids=" << text.identity.value << ',' << records.identity.value << ',' << again.identity.value
              << "\nC23 PASS operation_destructors=3 plan_destructors=9\nI01 PASS text=beta records=empty\n";
}
