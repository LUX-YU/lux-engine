param(
    [Parameter(Mandatory = $true)][string]$QualificationRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$SeedPak,
    [Parameter(Mandatory = $true)][string]$MissingGroundPak,
    [Parameter(Mandatory = $true)][string]$CxxPrefix,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Cost evidence requires a new directory' }
$qualification = Get-Content -LiteralPath "$QualificationRoot/qualification.json" -Raw | ConvertFrom-Json
if ($qualification.configuration -ne 'RelWithDebInfo' -or $qualification.diagnostics) {
    throw 'Costs require qualified normal RelWithDebInfo artifacts'
}
$source = $qualification.source
if ((& git -C $source rev-parse HEAD).Trim() -ne $qualification.source_commit -or
    (& git -C $source status --porcelain)) { throw 'Qualified source identity changed' }
$build = $qualification.build
$rules = Get-Content -LiteralPath "$build/build.ninja" -Raw
if ($rules -match 'LUX_SV1_DIAGNOSTICS=1|EditorAllocationDiagnostics\.cpp\.obj:|ClientAllocationDiagnostics\.cpp\.obj:|fsanitize=address') {
    throw 'Diagnostic instrumentation is forbidden in performance artifacts'
}
foreach ($artifact in $qualification.artifacts) {
    $file = Join-Path "$build/bin" $artifact.name
    if ((Get-FileHash -LiteralPath $file).Hash -ne $artifact.sha256) { throw "Artifact changed: $file" }
}
$pakHashes = @((Get-FileHash -LiteralPath $SeedPak).Hash, (Get-FileHash -LiteralPath $MissingGroundPak).Hash)
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:PATH = "$build/bin;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;$env:SystemRoot/System32;$env:SystemRoot"
$pairs = @()
foreach ($mode in @('resize', 'retry')) {
    $inputPak = if ($mode -eq 'retry') { $MissingGroundPak } else { $SeedPak }
    for ($pair = 1; $pair -le 5; ++$pair) {
        $order = if ($pair % 2) { @('legacy', 'scene') } else { @('scene', 'legacy') }
        foreach ($stack in $order) {
            if (Get-Process -Name lux_editor,lux_editor_er1,editor_workbench_cost,editor_scene_cost -ErrorAction SilentlyContinue) {
                throw 'Another desktop or cost process is running; stop measurement without killing it'
            }
            $sample = "$OutputDirectory/$mode-$pair-$stack.json"
            $executable = if ($stack -eq 'legacy') { 'editor_workbench_cost.exe' } else { 'editor_scene_cost.exe' }
            & "$build/bin/$executable" $inputPak $sample $mode $SeedPak *> "$OutputDirectory/$mode-$pair-$stack.log"
            if ($LASTEXITCODE -ne 0) { throw "Cost process $mode/$pair/$stack failed; sample not qualified" }
        }
        $old = Get-Content -LiteralPath "$OutputDirectory/$mode-$pair-legacy.json" -Raw | ConvertFrom-Json
        $new = Get-Content -LiteralPath "$OutputDirectory/$mode-$pair-scene.json" -Raw | ConvertFrom-Json
        foreach ($result in @($old, $new)) {
            if ($result.configuration -ne 'RelWithDebInfo' -or $result.warmup -ne 100 -or
                $result.view_count -ne 1 -or ($result.window -join ',') -ne '1600,900' -or
                $result.cadence_seconds -ne 0.008 -or !$result.close_completed -or
                $result.descriptors_created -ne $result.descriptors_retired) { throw 'Invalid process result' }
            $expectedStages = if ($mode -eq 'retry') { 'retry' } else { 'resize_down,resize_restore' }
            if (($result.stages.name -join ',') -ne $expectedStages) { throw 'Missing stages' }
            foreach ($stage in $result.stages) {
                $extent = if ($stage.name -eq 'resize_down') { '896,512' } else { '1024,576' }
                if ($stage.completed_scene_frames -ne 120 -or $stage.verification_frames -ne 8 -or
                    $stage.attempts -lt 120 -or ($stage.extent -join ',') -ne $extent -or
                    $stage.ready -ne 3 -or $stage.failed -ne 0 -or $stage.requests -ne 3 -or
                    $stage.handles -ne 12 -or !$stage.ready_frame -or !$stage.private_bytes_after) {
                    throw 'Dimensions, completion, readiness or resource accounting failed'
                }
                if ($mode -eq 'resize' -and $stage.serials_before -ne $stage.serials_after) {
                    throw 'Resize unexpectedly replaced a resource request'
                }
                if ($mode -eq 'retry' -and $stage.serials_after -le $stage.serials_before) {
                    throw 'Retry did not replace a request'
                }
            }
            if ($mode -eq 'retry' -and $result.provider_missing -lt 1) { throw 'Missing-asset negative not reached' }
            if ($result.stages[-1].checksum -ne '11872603163728535614') { throw 'Recovered target pixels differ' }
        }
        if ($old.initial_checksum -ne $new.initial_checksum) { throw 'Unequal initial content' }
        for ($i = 0; $i -lt $old.stages.Count; ++$i) {
            $a = $old.stages[$i]; $b = $new.stages[$i]
            if ($a.checksum -ne $b.checksum) { throw 'Unequal target pixels' }
            $pairs += [pscustomobject]@{
                mode = $mode; pair = $pair; stage = $a.name; frames = $a.completed_scene_frames
                checksum = $a.checksum; legacy_work_wall = $a.work_wall; candidate_work_wall = $b.work_wall
                legacy_wait_wall = $a.wait_wall; candidate_wait_wall = $b.wait_wall
                legacy_owner_cpu = $a.owner_cpu; candidate_owner_cpu = $b.owner_cpu
                legacy_process_cpu = $a.process_cpu; candidate_process_cpu = $b.process_cpu
                legacy_other_frames = $a.other_backend_frames; candidate_other_frames = $b.other_backend_frames
                legacy_private_bytes = $a.private_bytes_after; candidate_private_bytes = $b.private_bytes_after
                legacy_close_wall = $old.close_wall; candidate_close_wall = $new.close_wall
            }
        }
        $pairs | Export-Csv -LiteralPath "$OutputDirectory/pairs.csv" -NoTypeInformation
        Write-Output "$mode pair $pair PASS: equal View frame counts and actual target pixels; clean close"
    }
}
if ((Get-FileHash -LiteralPath $SeedPak).Hash -ne $pakHashes[0] -or
    (Get-FileHash -LiteralPath $MissingGroundPak).Hash -ne $pakHashes[1]) { throw 'Input pak changed' }
[ordered]@{
    source_commit = $qualification.source_commit; seed_sha256 = $pakHashes[0]; missing_ground_sha256 = $pakHashes[1]
    qualification_manifest_sha256 = (Get-FileHash -LiteralPath "$QualificationRoot/qualification.json").Hash
    artifacts = $qualification.artifacts; pairs = $pairs
    scope = 'Two scenarios, five independent process pairs each; hidden single-View production pipelines, not OS desktop input'
    accounting = 'Each stage has 120 View-bearing submissions, followed by eight readback verification frames. Maintenance/UI-only frames count separately but remain timed. Work includes owner observation; explicit wait includes pacing, polling and readback. Whole-process private/working-set bytes include drivers, caches and allocators; these are not GPU allocation bytes. Resource rows/handles and descriptor retirement are counted separately. Close is aggregate wall time.'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$OutputDirectory/identity.json" -Encoding utf8
