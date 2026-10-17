# N4 release correction after independent Debug failure

Candidate I `1dcef93b9156b317b3c2aa068ae86f0d7691e3c4` passed independent Release106/106,
but independent Debug104/106 failed. TextureSampling line336 and StorageBindings line128
both asserted zero allocation during last logical Storage release. Debug CRT abort dialogs
eventually caused the recorded CTest timeouts; these are failed assertions, not successful
cancellation. ASan was not started for this candidate. No N5 implementation proceeded.

The actual source moved the entire StorageSet::Record to defer its Native destructor until
the flat index was repaired. MSVC14.44 xstring move constructor allocates an iterator proxy
under _ITERATOR_DEBUG_LEVEL=2. A controlled standalone reproduction using the same string,
shared configuration and unique Native ownership measured one allocation for whole-record
move in Debug (zero in Release). Moving only the two ownership members and repairing the
dense slot by move assignment measured zero in both configurations. No GPU or IPC algorithm
is involved in this difference.

The correction moves Config and Native separately. Config is declared first so its pin
outlives Native destruction. The StorageId is removed and the dense index repaired before
the deleter executes. Record key strings are destroyed/move-assigned in place. No new
owner, retirement capacity, compatibility layer or exception handling was added.

The original zero-allocation assertions remain. Tests now report the separate Context and
Storage release counts. A Registry test removes a non-last record and checks the remaining
ID, owner, value and final exact destruction. Implementation full all build and second
no-work passed; affected six tests passed (3.39s) with GPU validation0 and release counts0.
Fresh independent Release/Debug/ASan from this correction's exact I must all pass before N4
closes. The failed candidate's complete logs remain in evidence/N4-owners; the corrected
verification uses a new clone/build/evidence tree, never edits that verification source.

Diagnostic repro, source/commands and logs: E:/RenderV2Rebuild/V12/string-move-repro.cpp and
evidence/n4-release-correction. Final V archives both candidate attempts. Performance remains
PARTIAL; the Debug-specific allocation explanation is not an overall rendering speed claim.
