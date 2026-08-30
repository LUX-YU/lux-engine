# ADR-20260830 — Typed Asset Framework and Cooked Envelope

- Date: 2026-08-30
- Status: Implemented
- Scope: Asset object model, typed SerDeser boundary, cooked-image identity

## Decision

Lux Asset runtime returns immutable concrete typed assets. `Asset` owns common metadata and auxiliary payload views;
`TAsset<T>` owns `shared_ptr<const T>`. Concrete final asset classes declare their canonical `AssetTypeId`, canonical name
and primary magic. Public typed decode no longer returns `shared_ptr<const void>` and does not use `TypeToken`.

All `.luxasset` images use the existing cooked-asset outer envelope. The Loader parses that envelope once from owned
`SharedBytes`, verifies that its non-null AssetId equals the AssetId requested from the Provider, and then invokes the
concrete typed SerDeser. A type-specific payload never encodes or synthesizes its own self AssetId. `AssetInfo.type` is
supplied by the concrete asset class; the outer `legacy_type_tag` is compatibility/validation data only.

World bundle identity/generation remain World payload identity and never substitute for AssetId. Transient domain values
without an assigned AssetId are not Assets.

## Retained mechanisms

AssetId, AssetTypeId, AssetVfs/Provider/Pak, OperationPort, SharedBytes, BinaryReader/BinaryWriter and all current size
budgets remain. World v2, Simulation and SceneDescription inner payload codecs retain their current explicit field encoding
and validation. Their complete files gain the generic envelope, but the inner World/Simulation payload bytes and Scene's
40-byte payload remain byte-identical.

## Selective restoration

Mature typed asset payload/codec/cooker logic may be ported from legacy into active packages. AssetManager, closed
`EAssetType`, filesystem-owning SerDeser, SerDeser factory, manager registration, load/unload state and compatibility shims
must not return.

The migration found no production consumer that knew only a magic/type and required runtime decoder discovery.
`AssetCodecSet`, `AssetCodecDescriptor`, `DecodedAsset`, codec contexts and the `TypeToken` payload dispatch were therefore
deleted after the final concrete descriptor producer disappeared. No typed/untyped adapter remains.

The active typed set now covers Texture, Shader, Mesh, Skeleton, AnimationClip, Material, MaterialInstance, Model,
TextureAtlas, FlipbookClip, ScriptArtifact, WorldDescription, SimulationDescription and SceneDescription. The mature
description codecs and rgbcx/bc7enc implementation were selectively moved into active private packages; none of the old
Manager/SerDeser-factory runtime returned.

## Held boundaries

This decision does not authorize Asset residency/demand, Asset services/context, ScriptSystem Asset injection, texture
streaming management or Full 3D Streaming.
