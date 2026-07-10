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

/// Mark a struct/class for reflection.
#define LUX_CLASS(...)    LUX_META(luxref::class, ##__VA_ARGS__)

/// Mark a struct/class as an ECS COMPONENT — a strict superset of
/// `LUX_CLASS()`. Expands to a `LUX_META` carrying the `luxref::class`
/// marker plus a `component=true` key/value pair. The meta code generator
/// notices this class-level flag and emits a registration block that pushes
/// the type into `lux::ecs::ComponentTypeRegistry` — the ECS-layer
/// catalogue the Hierarchy + Inspector iterate to auto-discover available
/// components. Non-component reflected types (e.g. struct payloads embedded
/// in components) stay on the plain `LUX_CLASS()`.
///
/// Intentionally non-variadic: libclang's parser tooling does not reliably
/// honour the `, ##__VA_ARGS__` GCC extension when the macro mixes
/// fixed-position arguments with variadic ones, so users can't pass extra
/// `key=value` pairs at the call site. Add per-component knobs as
/// dedicated annotations or member-level `LUX_MEMBER` keys instead.
#define LUX_COMPONENT()   LUX_META(luxref::class, component=true)

/// Mark a free function for reflection.
#define LUX_FUNC(...)     LUX_META(luxref::function, ##__VA_ARGS__)

/// Mark an enum for reflection.
#define LUX_ENUM(...)     LUX_META(luxref::enum, ##__VA_ARGS__)

/// Mark a property (generic).
#define LUX_PROPERTY(...) LUX_META(luxref::property, ##__VA_ARGS__)

/// Mark a function parameter.
#define LUX_PARAM(...)    LUX_META(luxref::property::param, ##__VA_ARGS__)

/// Mark a struct/class member field for inspector display.
/// Supported annotation keys:
///   display_name=<string>   Label shown in the inspector (default: field name).
///   min=<float>             Minimum value for slider/drag widgets.
///   max=<float>             Maximum value for slider/drag widgets.
///   tooltip=<string>        Tooltip shown on hover.
///   color=true              Treat an Eigen::Vector3f/4f as a colour picker.
///   readonly=true           Show the field but prevent editing.
///   labels=<a>,<b>,…        Per-component axis labels for vector fields
///                           (e.g. labels=R,G,B overrides the default X/Y/Z).
#define LUX_MEMBER(...)   LUX_META(luxref::property::member, ##__VA_ARGS__)

/// Explicitly exclude a public member from reflection.
/// Place this macro on any public field or method that should be invisible to
/// the reflection system and the inspector, even though it is public.
/// Example:
///   bool LUX_NO_MEMBER() dirty{false};
#define LUX_NO_MEMBER()   LUX_META(luxref::property::skip)

/// Mark a method for reflection.
#define LUX_METHOD(...)   LUX_META(luxref::property::method, ##__VA_ARGS__)
