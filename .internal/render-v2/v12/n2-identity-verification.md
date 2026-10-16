# N2 identities and stable compile borrows — independent verification

Implementation I: `35367c42fd1949526157b60ed1b6d54f0f933597`.
Parent: `131162ce6fcdc78a15996b1a50478d33067417b7`.

The detached clean no-hardlinks clone at E:/RenderV2Rebuild/V12/n2-identity-verification
has no alternates and no source changes. Tracked snapshot, fresh MSVC ASan configuration,
full all -j 4 -- -k 0, second no-work build and 105/105 tests passed (247.75 seconds).
ASAN_OPTIONS=alloc_dealloc_mismatch=1. No ASan or Vulkan Validation/SyncValidation errors
were reported; all actual output, including warnings, is retained in the evidence index.
This includes stable Feature/Storage type growth, complete original Render/Platform tests,
installed consumers and real GPU lifecycle tests. Test duration is not a performance result.

Protection passed: 719 Legacy blobs, three user files and dependency identities.
N2-IDENTITY-evidence-index.json identifies commands, logs and protection output by SHA-256.
The runner is E:/RenderV2Rebuild/V12/verify-n2-identity.ps1.

This qualifies this checkpoint only. Handler ownership, installation transaction, typed Reply,
Feature config/selection migration and removal of the old GPU completion bridge remain open.
N2/N3 are not yet qualified. Historical performance remains PARTIAL; upper products NOT_RUN.
