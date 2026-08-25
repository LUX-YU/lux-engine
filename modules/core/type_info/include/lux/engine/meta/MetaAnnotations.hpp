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
#define LUX_TYPE_INFO(mode, ...) \
    LUX_META(luxref::class, type_info=#mode, ##__VA_ARGS__) \
    LUX_META(luxtype::type, projection=#mode, ##__VA_ARGS__)

/// Mark a free function for reflection.
#define LUX_FUNC(...)     LUX_META(luxref::function, ##__VA_ARGS__)

/// Mark an enum for reflection.
#define LUX_ENUM_INFO(mode, ...) \
    LUX_META(luxref::enum, type_info=#mode, ##__VA_ARGS__) \
    LUX_META(luxtype::enum, projection=#mode, ##__VA_ARGS__)

/// A field participates in TypeStaticInfo unless skip_static=true; the runtime
/// projection likewise honours skip_runtime=true.
#define LUX_TYPE_MEMBER(...) \
    LUX_META(luxtype::member, ##__VA_ARGS__)

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

// ============================================================================
//  PassParams annotations (marker family `luxpass`, disjoint from `luxref`)
//
//  Consumed by the render PassParams generator (engine_add_pass_params(), two
//  inja templates rendering a C++ .pass.hpp and a GLSL .lglslh from one
//  annotated header). The marker split matters: the params struct holds
//  Vulkan/graph handle fields that must stay INVISIBLE to editor reflection,
//  so it carries only `luxpass`; the scalars struct is shared with the editor
//  panel, so it carries BOTH markers (LUX_TYPE_INFO(runtime) + LUX_PASS_SCALARS()).
//
//  Parser constraint (CxxParserImpl isFromMainFile guard): the scalars struct
//  must be defined in the SAME header as the params struct — fields of records
//  from included headers are not visible to the generator's translation unit.
// ============================================================================

/// Mark a struct as a render-pass parameter block (the generator's root).
/// Exactly one per header (v0): the .lglslh binding order is derived from
/// this struct's resource-field declaration order.
#define LUX_PASS_PARAMS()   LUX_META(luxpass::params)

/// Mark the scalar sub-struct (push-constant段). Combine with
/// LUX_TYPE_INFO(runtime) when
/// the same struct feeds the editor panel.
#define LUX_PASS_SCALARS()  LUX_META(luxpass::scalars)

/// Mark a resource field inside a LUX_PASS_PARAMS() struct. Key/value only
/// (bare tokens don't survive annotation parsing reliably):
///   role=read      sampled texture input  (RGResourceHandle; needs glsl=)
///   role=sampler   VkSampler paired with a read field (needs for=<field>)
///   role=write     color-attachment output (RGResourceHandle)
///   glsl=<uName>   GLSL uniform identifier emitted into the .lglslh
///   for=<field>    sampler pairing: the read field this sampler serves
#define LUX_RESOURCE(...)   LUX_META(luxpass::resource, ##__VA_ARGS__)

// ============================================================================
//  Comm-ops annotations (marker family `luxop`, disjoint from luxref/luxpass)
//
//  A feature declares its ENTIRE comm surface in one header — creation config,
//  op payloads, kinds, reply types, blob fields — and engine_add_comm_ops()
//  generates the whole Operation face from it: op descriptors, CommandTraits
//  specialisations, FeatureOpIds, the payload-taking Proxy (decl + impl),
//  FeatureOpRegistrar/descriptor/factory and the same-name-field createFn.
//  Hand-written remainder: the per-op handler bodies (`handle<OpName>` by
//  convention — declared by the generated .ops.cpp, defined by the feature)
//  — they ARE the feature's semantics. Transport routing needs no vocabulary
//  here at all: send/sendWithReply/sendBulk/sendBlob are picked from kind and
//  traits (FeatureOpSend.hpp), the generator just spells the right verb.
// ============================================================================

/// Mark the creation-config struct AND carry the feature's comm identity.
/// Keys (kv-only):
///   prefix=<Ident>        names everything: <Ident>Proxy / <Ident>OperationIds
///                         / k<Ident>FeatureFactory / <ident>CreateFn
///   id=<dotted.id>        FeatureDescriptor type id (featureId("..."))
///   display=<Name>        FeatureDescriptor/Factory display name
///   feature=<CppType>     server feature class for addFeature<T> in createFn
///   feature_header=<path> include path of that class for the generated .cpp
///   custom_create=true    skip createFn generation (declared extern instead)
///                         — for configs whose Feature::Config mapping is not a
///                         same-name field copy (§7.5: 非同构不硬并)
///   requires=<dotted.id>  hard feature dependency → FeatureDependency table on
///                         the generated descriptor (absent dep = explicit
///                         reject, never a silent no-op feature)
///   param_op=<Name>       append a generic reflected-setParams op (EOpKind::
///                         Param, shared SetFeatureParamsPayload, no proxy
///                         method) as the LAST op — param_set_op_index derives
///                         from its position automatically
///   no_factory=true       generate ONLY the client face (ops/traits/OpIds/
///                         Proxy); createFn/registrar/descriptor/factory stay
///                         hand-written — for multi-factory features (e.g.
///                         PointCloud's five render modes sharing one op set).
///                         id=/display=/feature= become optional.
#define LUX_COMM_CONFIG(...) LUX_META(luxop::config, ##__VA_ARGS__)

/// Mark an op payload struct. Keys:
///   kind=stream|resource|bulk|blob|param   dispatch shape (EOpKind)
///   name=<DispatcherName>                  op name (server dispatcher key)
///   method=<proxyMethod>                   generated Proxy method name
///   reply=<CppType>                        reply POD → CommandTraits spec +
///                                          RenderRequest<reply> return
#define LUX_OP(...)          LUX_META(luxop::op, ##__VA_ARGS__)

/// Mark the BlobRef member inside a kind=blob payload — becomes the op
/// descriptor's `blob_field` pointer-to-member (sendBlob writes it).
#define LUX_OP_BLOB()        LUX_META(luxop::blob_field)
