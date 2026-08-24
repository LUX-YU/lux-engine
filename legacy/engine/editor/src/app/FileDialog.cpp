#include <lux/engine/editor/app/FileDialog.hpp>

// nfd.hpp is the upstream C++ RAII wrapper (NFD::Guard / NFD::UniquePathU8).
// We always drive the UTF-8 (U8) API so paths are platform-uniform UTF-8; on
// Windows nfd's "native" strings are wchar_t while U8 stays char, and the U8
// overloads alias the native ones on platforms where they are the same type, so
// this single code path compiles everywhere.
#include <nfd.hpp>

namespace lux::editor
{
    namespace
    {
        // nfd's U8 strings are UTF-8. std::filesystem::path's narrow ctor decodes
        // bytes in the active code page on Windows, which corrupts non-ASCII
        // paths — go through char8_t (the UTF-8 ctor) for a correct conversion on
        // every platform.
        std::filesystem::path fromNfdU8(const nfdu8char_t* s)
        {
            return std::filesystem::path(reinterpret_cast<const char8_t*>(s));
        }

        // std::filesystem::path -> UTF-8 std::string for handing back to nfd.
        std::string toNfdU8(const std::filesystem::path& p)
        {
            const std::u8string s = p.u8string();
            return std::string(s.begin(), s.end());
        }

        // Build an nfdwindowhandle_t from the engine's opaque native handle so
        // the dialog is owner-modal (parented). Only Windows is wired today — the
        // engine only exposes win32Handle(); elsewhere we pass an unset handle,
        // which nfd treats as "no parent".
        nfdwindowhandle_t makeParent(void* native)
        {
            nfdwindowhandle_t h{};
#ifdef _WIN32
            if (native)
            {
                h.type   = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
                h.handle = native;
            }
#else
            (void)native;
#endif
            return h;
        }

        // The returned items point into `filters`; keep `filters` alive across the
        // dialog call (callers always do — it is a function argument).
        std::vector<nfdu8filteritem_t> makeFilters(
            const std::vector<FileFilter>& filters)
        {
            std::vector<nfdu8filteritem_t> items;
            items.reserve(filters.size());
            for (const auto& f : filters)
                items.push_back({ f.name.c_str(), f.spec.c_str() });
            return items;
        }
    } // namespace

    std::optional<std::filesystem::path> openFileDialog(
        void*                          parent_native_window,
        const std::vector<FileFilter>& filters,
        const std::filesystem::path&   default_path)
    {
        NFD::Guard nfd;   // RAII: NFD_Init() now, NFD_Quit() on scope exit.

        const auto items  = makeFilters(filters);
        const auto def    = toNfdU8(default_path);
        const auto parent = makeParent(parent_native_window);

        NFD::UniquePathU8 out;
        const nfdresult_t r = NFD::OpenDialog(
            out,
            items.empty() ? nullptr : items.data(),
            static_cast<nfdfiltersize_t>(items.size()),
            default_path.empty() ? nullptr : def.c_str(),
            parent);

        if (r == NFD_OKAY)
            return fromNfdU8(out.get());
        return std::nullopt;   // NFD_CANCEL or NFD_ERROR
    }

    std::optional<std::filesystem::path> saveFileDialog(
        void*                          parent_native_window,
        const std::vector<FileFilter>& filters,
        const std::filesystem::path&   default_path,
        const std::string&             default_name)
    {
        NFD::Guard nfd;

        const auto items  = makeFilters(filters);
        const auto def    = toNfdU8(default_path);
        const auto parent = makeParent(parent_native_window);

        NFD::UniquePathU8 out;
        const nfdresult_t r = NFD::SaveDialog(
            out,
            items.empty() ? nullptr : items.data(),
            static_cast<nfdfiltersize_t>(items.size()),
            default_path.empty() ? nullptr : def.c_str(),
            default_name.empty() ? nullptr : default_name.c_str(),
            parent);

        if (r == NFD_OKAY)
            return fromNfdU8(out.get());
        return std::nullopt;
    }

    std::optional<std::filesystem::path> pickFolderDialog(
        void*                        parent_native_window,
        const std::filesystem::path& default_path)
    {
        NFD::Guard nfd;

        const auto def    = toNfdU8(default_path);
        const auto parent = makeParent(parent_native_window);

        NFD::UniquePathU8 out;
        const nfdresult_t r = NFD::PickFolder(
            out,
            default_path.empty() ? nullptr : def.c_str(),
            parent);

        if (r == NFD_OKAY)
            return fromNfdU8(out.get());
        return std::nullopt;
    }
}
