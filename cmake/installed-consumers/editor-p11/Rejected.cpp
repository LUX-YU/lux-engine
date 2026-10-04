#include <cstdlib>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#if defined(_WIN32)
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif
using namespace lux::editor;
namespace
{
    extensions::ContributionResult<void> mustNotCall(extensions::ContributionDraft&, lux::object::CodeLease)
    {
        std::abort();
    }
} // namespace
#if PROBE_REJECT == 6
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v6() noexcept
#elif PROBE_REJECT == 11
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v9() noexcept
#else
extern "C" PROBE_EXPORT const extensions::EditorExtensionExports* lux_editor_exports_v10() noexcept
#endif
{
    static const auto exports = []
    {
        extensions::EditorExtensionExports value;
        value.contribute = &mustNotCall;
#if PROBE_REJECT == 6
        value.interface_version = 6;
#elif PROBE_REJECT == 11
        value.interface_version = 9;
#elif PROBE_REJECT == 7
        value.interface_version = 7;
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
