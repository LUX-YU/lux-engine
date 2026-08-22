# Unfinished Architecture Work

This ledger tracks implementation state only. Decisions live in ADRs.

## Active: semantic deduplication (2026-08-22)

- [x] Freeze the architecture boundary and legacy-vocabulary baseline.
- [x] Introduce the ECS-free typed async port and remove ECS Runtime includes.
- [x] Replace Extension ABI v4 with v5 and make Physics2D optional.
- [x] Publish reflection and component projection as one module transaction.
- [ ] Remove SceneFeature, SceneContribution and Runtime Pack identities.
- [ ] Reduce render extraction to one RenderSystem and a private static stage
      sequence; remove Runtime RenderEffect.
- [ ] Move every Runtime-owned ISystem and Component to its ECS domain.
- [ ] Split `engine/spatial3d/SceneCatalog` by field ownership.
- [ ] Collapse Editor panel contribution state into UISystem registration.
- [ ] Generate build-only project usage and direct game composition.
- [ ] Remove legacy paths/targets and set all semantic-debt limits to zero.
- [ ] Complete Windows profile, installed-prefix and Android validation.

The working tree modification under
`modules/function/input/pinclude/lux/engine/input/detail/GlfwInputTranslation.hpp`
predates this work and is not part of the refactor.
