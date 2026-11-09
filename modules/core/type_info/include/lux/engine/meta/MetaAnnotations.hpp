#pragma once
// ============================================================================
//  MetaAnnotations.hpp
//  Lightweight annotation macros for Lux reflection.
//
//  This header has ZERO compile-time overhead:
//    - At regular compilation all macros expand to nothing.
//    - At parse time (__LUX_PARSE_TIME__ defined by the meta code generator)
//      they expand to Clang __attribute__((annotate(...))).
//
//  Include this header (instead of the heavier Meta.hpp) in game-domain
//  component headers that only need annotation markers and no runtime data.
// ============================================================================
#include <lux/cxx/reflection/runtime/Marker.hpp>

/// Select compile-time, runtime, or both type-information projections.
/// The runtime projection temporarily retains the established luxref marker
/// family while the generator consumes a separate luxtype projection marker.
#define LUX_TYPE_INFO(mode, ...)                                                                                       \
    LUX_META(luxref::class, type_info = #mode, ##__VA_ARGS__)                                                          \
    LUX_META(luxtype::type, projection = #mode, ##__VA_ARGS__)

/// Mark a free function for reflection.
#define LUX_FUNC(...) LUX_META(luxref::function, ##__VA_ARGS__)

/// Mark an enum for reflection.
#define LUX_ENUM_INFO(mode, ...)                                                                                       \
    LUX_META(luxref::enum, type_info = #mode, ##__VA_ARGS__)                                                           \
    LUX_META(luxtype::enum, projection = #mode, ##__VA_ARGS__)

/// A field participates in TypeStaticInfo unless skip_static=true; the runtime
/// projection likewise honours skip_runtime=true.
#define LUX_TYPE_MEMBER(...) LUX_META(luxtype::member, ##__VA_ARGS__)

/// Mark a property (generic).
#define LUX_PROPERTY(...) LUX_META(luxref::property, ##__VA_ARGS__)

/// Mark a function parameter.
#define LUX_PARAM(...) LUX_META(luxref::property::param, ##__VA_ARGS__)

/// Mark a field for structural/runtime reflection. Domain-specific attributes
/// are interpreted only by their owning projection, never by type_info.
#define LUX_MEMBER(...) LUX_META(luxref::property::member, ##__VA_ARGS__)

/// Explicitly exclude a public member from reflection.
/// Place this macro on any public field or method that should be invisible to
/// the runtime reflection projection, even though it is public.
/// Example:
///   bool LUX_NO_MEMBER() dirty{false};
#define LUX_NO_MEMBER() LUX_META(luxref::property::skip)

/// Mark a method for reflection.
#define LUX_METHOD(...) LUX_META(luxref::property::method, ##__VA_ARGS__)
