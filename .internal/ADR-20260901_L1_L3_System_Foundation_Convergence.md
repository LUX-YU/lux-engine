# ADR: Shared System Foundation and SimulationSystem Hard Cut

- Status: Accepted
- Date: 2026-09-01
- Implementation: `9e78c377fb50521c36419185a5aefb53a6db15e9`

## Decision

Stable System type/instance identity, common type description and deterministic dependency ordering belong to the narrow
`engine/domain/system` DOMAIN foundation. Runtime objects, access metadata, installers and the registry remain specific to
`lux::simulation::SimulationSystem` or the later `lux::scene::SceneSystem` contracts.

Simulation uses the explicit `SimulationSystem*` vocabulary without forwarding headers or aliases. Registration metadata
has one version/multiplicity SSOT in `SimulationSystemDescription::type`, and configuration decode uses the sole L0
`serialization::PortableValueCodec` path. Transform configuration is reflected and field-wise portable rather than a
hand-written native-layout payload.

Simulation wire version 6 appends the common multiplicity value to each canonical System type record. Version 5 and older
payloads fail closed and repository fixtures are recooked. Stable canonical System names are unchanged.

## Evidence

- RelWithDebInfo `all -j 4 -k 0`: passed; second build reported `ninja: no work to do`.
- VS Developer PowerShell CTest: 143/143 passed.
- Installed consumers passed: system foundation, core system and simulation runtime.
- Simulation v6 inner payload golden: 873 bytes,
  SHA-256 `480ed360a99aac9ac125915739e38469896c53bd9d14f76efaac8b6b8f4ac8f3`.

## Held

This decision does not introduce SceneSystem runtime composition, plugin hot reload, a service registry, a generic
scheduler, multi-task Systems or Asset residency. Those remain governed by their own convergence waves or held decisions.
