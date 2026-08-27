#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

// ---------------------------------------------------------------------------
// Force-export the VMA entry points that downstream binaries import from
// render.dll (the editor's EditorRenderServer performs a readback via the full
// VMA API). VMA's public functions are emitted as COMDAT definitions, which
// CMake's WINDOWS_EXPORT_ALL_SYMBOLS (.def auto-generation) skips — so without
// these directives the symbols satisfy render's import lib at link time but are
// absent from render.dll's actual export table, producing a runtime
// "entry point not found" when the consumer DLL/EXE loads. Keep in sync with
// the VMA API the editor calls.
#if defined(_MSC_VER)
#pragma comment(linker, "/export:vmaCreateBuffer")
#pragma comment(linker, "/export:vmaDestroyBuffer")
#pragma comment(linker, "/export:vmaCreateImage")
#pragma comment(linker, "/export:vmaDestroyImage")
#pragma comment(linker, "/export:vmaMapMemory")
#pragma comment(linker, "/export:vmaUnmapMemory")
#pragma comment(linker, "/export:vmaFlushAllocation")
#pragma comment(linker, "/export:vmaInvalidateAllocation")
#endif
