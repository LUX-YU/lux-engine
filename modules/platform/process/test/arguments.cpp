#include <lux/engine/platform/Process.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <thread>

int main(int argc, char** argv)
{
    using namespace lux::engine::platform;
    const auto values = processArguments(argc, argv);
    assert(values);
    if (values->size() >= 3 && (*values)[1] == "--child")
    {
        const auto output = std::filesystem::u8path((*values)[2]);
        {
            std::ofstream stream(output, std::ios::binary);
            assert(stream);
            for (std::size_t index = 3; index < values->size(); ++index)
                stream << std::quoted((*values)[index]) << '\n';
        }
        std::ofstream(output.string() + ".done").put('1');
        return 0;
    }
    assert(values->size() == 2);
    auto executable = executablePath();
    assert(executable && executable->is_absolute());
    const auto directory = std::filesystem::u8path((*values)[1]);
    std::filesystem::create_directories(directory);
    const auto file =
        directory / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    const auto utf8 = file.u8string();
    const std::vector<std::string> payload{"", "with spaces", "中文项目", "quoted\"value", "trailing\\", "\\\"mixed\\"};
    std::vector<std::string> arguments{"--child", std::string(utf8.begin(), utf8.end())};
    arguments.insert(arguments.end(), payload.begin(), payload.end());
    assert(launchProcess(*executable, arguments));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(file.string() + ".done"))
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::ifstream stream(file, std::ios::binary);
    for (const auto& expected : payload)
    {
        std::string actual;
        assert(stream >> std::quoted(actual));
        assert(actual == expected);
    }
    const std::vector<std::string> invalid{std::string("embedded\0null", 13)};
    assert(!launchProcess(*executable, invalid));
    assert(!launchProcess("relative.exe", {}));
    assert(!launchProcess(directory / "missing-executable", {}));
}
