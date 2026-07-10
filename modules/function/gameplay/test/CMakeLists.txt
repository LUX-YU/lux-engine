# ─────────────────────────────────────────────────────────────────────────────
#  gameplay/test — test policy (2026-07-06)
#
#  Functional-verification tests are DEVELOPMENT-TIME artifacts here: they gate a
#  change while it is being built (including mutation verification) and are
#  DELETED once the change has landed green. Only stress / performance targets
#  and interactive demos stay in-tree permanently — gameplay currently has none.
#
#  HeadlessBridgeFixture.hpp is kept: it is development INFRASTRUCTURE (a real
#  RenderSession + generic fake RenderServer + recording dispatcher for headless,
#  GPU-free bridge round-trips), not a test. The upcoming 2D slices (F2-08
#  PixelField2DBridge, A2-02 Tilemap2DBridge) drive their development-time tests
#  through it, exactly as the Gate -1 / Slice A bridge tests did.
# ─────────────────────────────────────────────────────────────────────────────



