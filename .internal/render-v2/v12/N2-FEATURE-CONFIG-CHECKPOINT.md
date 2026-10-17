# FeatureId selection and owned typed configuration checkpoint I

N2/N3 remains IN_PROGRESS. Production CreateEffect now carries FeatureUse {FeatureId, local instance,
ConfigValue}. Owner-thread admission resolves the generational ID directly in the existing stable
Feature table, then hands stable entries plus the owning request to compilation. No name/version scan,
FeatureSelection compatibility alias, parallel lookup directory or new owner. Tests resolve presets
through real ListFeatures at cold boundaries; the test-only TestClient cache does not enter production.

FeatureDescriptor carries the existing ConfigValue's complete TypeToken/size/alignment qualification.
Assembly compares it once before describe/build; callbacks receive a trusted immutable borrow.
Program callbacks use the same proven input. ConfigValue owns the actual C++ value and source pin,
with precise full-backing accounting; no encoded Feature Schema or manual Feature version remains.
ConfigValue reuses its original shared owner, not a second Any/SBO. Source pins survive dependent
config destruction, including assignment. FeatureUse has no second generation or resource identity.

Removed the separate CreateEffect attachment vector: an owned config retains its actual RenderPayload.
The 64-visible-byte/1-MiB backing test preserves original pointer, budget rejection/refund, abandon,
close during worker callback and exactly-once destruction. Explicit Server config byte budgets no
longer inherit G2's default wire ceiling. G2 persistent Schema/Wire negative assertions remain tested
at their actual data boundary, and specialized Storage binding codecs remain pending N5 migration.
Unused Offscreen/Texture/external View Feature-only fixture codecs are removed; their business value
checks remain in describe. Assembly public headers no longer transitively require Encoding.

Zero-Pass Features may omit programs, but an actual zero-Pass parameter contributor is called and
its values are adopted. Equal arguments remain accepted, conflicting zero-Pass parameters fail without
replacing last-good. Pass authorship audit stays unique; this does not yet implement N5 live-only
lowering or final parameter LayoutPlan. Stale Feature generation fails before compile admission.
Cross-DLL Feature compilation, external Buffer, four Views and real GPU regressions remain.

Full all -j 4 -- -k 0 and second no-work PASS; focused 9/9 (8.86 s), complete 106/106 (118.53 s).
Measured ConfigValue=48, FeatureUse=88, CreateEffectRequest=48 bytes. No speed/equivalence conclusion.
First build found one old Wire-limit argument and two missing direct test includes; corrected in
implementation, failures retained. No Queue, IPC, Transfer, GPU retirement or LastUse algorithm change.
Independent V pending at exact I. Hello bootstrap narrowing still required before joint N2/N3 closes.
N4-N7 NOT_STARTED; performance PARTIAL; root product historical BLOCKED; upper products and unrun
platforms NOT_RUN. Three-prefix final synchronization pending; no push.
