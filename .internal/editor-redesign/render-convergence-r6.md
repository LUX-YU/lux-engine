# Architecture convergence R6 frozen verification input

Parent / last production implementation: `ed975321ce348e4d30ad13296bf0a483ffc79bc2` (R5).
This commit adds only the combined R0-R5 protection/shape checker and this verification input note.
It does not claim independent verification has already passed. V will record this exact commit and all results.

Source baseline is b05904dc71f91be2c89964461430ad924ee97e2a. R0-R5 notes/checkers remain immutable at their own
commits. Combined whitelist is the exact union of those work blocks plus these two verification inputs.
All other baseline files, Legacy719, IPC Channel/Link/SPSC algorithms and original user changes remain protected.
The primary dirty E:/SyncForder/CodeRepos/lux-engine worktree is not the construction or verification checkout.

Final matrix: independent git clone --no-hardlinks --no-checkout, detached exact I, no alternates; tracked snapshot
before configuration. Serial RelWithDebInfo, Debug and ASan configure/all -j 4 -- -k 0/second no-work/CTest -j 1.
Full applicable Render and Platform tests, real GPU Validation/SyncValidation, installed relocated consumers,
standalone headers, Client-only with Vulkan discovery disabled, three-prefix committed header/hash audit and
root TOOLCHAIN real configure. The root failure is a separate product qualification, not masked by module PASS.
Performance raw samples, environment, timing ranges and actual Native call counts are archived outside source
under E:/RenderV2Rebuild/ConvergenceR1/verification/<I SHA>, with a content hash manifest.

A failing required correctness gate must be repaired in the implementation worktree and a new I validated from
scratch. Verification source never receives production fixes. V adds only the final report/evidence index.
Default push authorization applies after final audit, without force or main merge. Stop before G10-G13.
IPC historical PARTIAL unchanged. Linux/Android/TSan NOT_RUN and GPU timestamp timing NOT_MEASURED.
