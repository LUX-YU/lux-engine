# ─────────────────────────────────────────────────────────────────────────────
#  check_public_sdk_includes.cmake — run in script mode (cmake -P).
#
#  ADR (lux-engine-pack-architecture §2/§9): a pack may include ONLY the public
#  SDK of a module (its include/ tree). Module-private headers (sinclude/,
#  pinclude/) are visible at compile time — the engine's own targets share them
#  through BUILD_TIME_SHARED include dirs — so nothing but this check stops a
#  pack from reaching into them. Reaching in would silently destroy the very
#  property the packs tier exists for: that pack_d2 / pack_d3 compile exactly
#  like a third-party pack, and therefore prove the public extension surface is
#  sufficient.
#
#  Detection: for every #include <lux/...> in a pack source, the named header
#  must NOT exist under any module's sinclude/ or pinclude/ tree. (Matching by
#  the header's REAL location, not by the spelling — a private header included
#  as <lux/engine/render/foo.hpp> is caught the same way.)
#
#  Args: -DPACKS_DIR=<abs> -DREPO_ROOT=<abs> [-DSTAMP=<file>]
# ─────────────────────────────────────────────────────────────────────────────

if(NOT PACKS_DIR OR NOT REPO_ROOT)
    message(FATAL_ERROR "[packs-sdk-check] PACKS_DIR and REPO_ROOT are required")
endif()

# Collect every module-private header, keyed by its <lux/...> include spelling.
file(GLOB_RECURSE _private_headers
     "${REPO_ROOT}/modules/*/*/sinclude/*.hpp"
     "${REPO_ROOT}/modules/*/*/sinclude/*.h"
     "${REPO_ROOT}/modules/*/*/pinclude/*.hpp"
     "${REPO_ROOT}/modules/*/*/pinclude/*.h")

set(_private_spellings "")
foreach(_h IN LISTS _private_headers)
    # .../<module>/(s|p)include/<spelling>  →  <spelling>
    string(REGEX REPLACE ".*/[sp]include/" "" _spelling "${_h}")
    list(APPEND _private_spellings "${_spelling}")
endforeach()
list(REMOVE_DUPLICATES _private_spellings)

file(GLOB_RECURSE _pack_sources
     "${PACKS_DIR}/*.hpp" "${PACKS_DIR}/*.cpp" "${PACKS_DIR}/*.h" "${PACKS_DIR}/*.inl")

set(_violations "")
foreach(_src IN LISTS _pack_sources)
    file(STRINGS "${_src}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(_line IN LISTS _lines)
        string(REGEX MATCH "[<\"]([^>\"]+)[>\"]" _m "${_line}")
        set(_inc "${CMAKE_MATCH_1}")
        if(_inc IN_LIST _private_spellings)
            file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_src}")
            list(APPEND _violations "  ${_rel}: #include <${_inc}>  (module-private header)")
        endif()
    endforeach()
endforeach()

if(_violations)
    list(JOIN _violations "\n" _msg)
    message(FATAL_ERROR
        "[packs-sdk-check] packs may only include the PUBLIC SDK (module include/ trees).\n"
        "Module-private headers (sinclude/ pinclude/) reached from packs:\n${_msg}\n"
        "Fix: promote the needed API into the module's public include/ tree, or keep the\n"
        "logic inside the module. See .internal/lux-engine-pack-architecture-adr.md §2/§9.")
endif()

list(LENGTH _pack_sources _n)
message(STATUS "[packs-sdk-check] ${_n} pack sources: public SDK only — OK")
if(STAMP)
    file(WRITE "${STAMP}" "ok")
endif()
