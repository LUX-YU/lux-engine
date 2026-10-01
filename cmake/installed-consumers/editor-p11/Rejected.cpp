#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <cstdlib>
#if defined(_WIN32)
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif
using namespace lux::editor;
namespace
{
    extensions::ContributionResult<void> mustNotCall(extensions::ContributionDraft&, contracts::CodeLease)
    {
        std::abort();
    }
}
#if PROBE_REJECT == 6
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v6() noexcept
#else
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v7() noexcept
#endif
{
    static const auto exports = [] {
        extensions::EditorExtensionExports value;
        value.contribute = &mustNotCall;
#if PROBE_REJECT == 6 || PROBE_REJECT == 7
        value.interface_version = 6;
#elif PROBE_REJECT == 8
        value.structure_size -= 1;
#elif PROBE_REJECT == 9
        value.editor_sdk_abi = "incompatible";
#else
        value.counts.views = 257;
#endif
        return value;
    }();
    return &exports;
}
