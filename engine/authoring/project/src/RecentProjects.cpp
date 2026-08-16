#include <lux/engine/authoring/project/RecentProjects.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

namespace lux::authoring
{
    std::filesystem::path recentProjectsPath()
    {
        if (const char* appdata = std::getenv("APPDATA"); appdata)
            return std::filesystem::path(appdata) / "Lux" / "recent.txt";
        if (const char* home = std::getenv("HOME"); home)
            return std::filesystem::path(home) / ".config" / "lux" / "recent.txt";
        return std::filesystem::current_path() / ".lux_recent.txt";
    }

    std::vector<std::filesystem::path> loadRecentProjects()
    {
        std::vector<std::filesystem::path> out;
        std::ifstream f(recentProjectsPath());
        std::string   line;
        while (std::getline(f, line))
        {
            if (!line.empty())
                out.emplace_back(line);
        }
        return out;
    }

    void saveRecentProjects(const std::vector<std::filesystem::path>& list)
    {
        const auto path = recentProjectsPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        for (const auto& p : list)
            f << p.string() << '\n';
    }

    void pushRecentProject(const std::filesystem::path& p)
    {
        // Canonicalise to absolute so the editor's write and the
        // launcher's read agree even if either was launched with a
        // relative path.
        std::error_code ec;
        const auto abs_p = std::filesystem::absolute(p, ec);
        const auto& canonical = ec ? p : abs_p;

        auto list = loadRecentProjects();
        // Move-to-front: remove existing entries with the same path,
        // then push to the front.
        list.erase(std::remove_if(list.begin(), list.end(),
            [&](const std::filesystem::path& q){ return q == canonical; }),
            list.end());
        list.insert(list.begin(), canonical);
        if (list.size() > kRecentProjectsCap)
            list.resize(kRecentProjectsCap);
        saveRecentProjects(list);
    }

} // namespace lux::authoring
