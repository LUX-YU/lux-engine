param(
    [Parameter(Mandatory = $true)][string]$NormalBuild,
    [Parameter(Mandatory = $true)][string]$DiagnosticBuild,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$PdbUtil
)
$ErrorActionPreference = 'Stop'
$normalRoot = (Resolve-Path -LiteralPath $NormalBuild).Path
$diagnosticRoot = (Resolve-Path -LiteralPath $DiagnosticBuild).Path
if ($normalRoot -eq $diagnosticRoot) { throw 'Normal and diagnostic products must use separate build trees' }
foreach ($entry in @(@($normalRoot, 'OFF'), @($diagnosticRoot, 'ON'))) {
    $cache = Get-Content -LiteralPath (Join-Path $entry[0] 'CMakeCache.txt') -Raw
    if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\r?$') { throw 'Only RelWithDebInfo is qualified' }
    if ($cache -notmatch ('(?m)^LUX_EDITOR_DIAGNOSTICS:BOOL=' + $entry[1] + '\r?$')) { throw 'Diagnostic cache identity mismatch' }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$normalNinja = Get-Content -LiteralPath (Join-Path $normalRoot 'build.ninja') -Raw
if ($normalNinja -match '(?i)fsanitize=address|asan_(dynamic|runtime)') {
    throw 'Address-check instrumentation must not enter the normal SDK/performance build'
}
if ($normalNinja -match 'LUX_SV1_DIAGNOSTICS=1') {
    throw 'Legacy workbench diagnostics must not enter the normal SDK/performance build'
}
if ($normalNinja -match 'build [^\r\n]*EditorAllocationDiagnostics\.cpp\.obj:' -or
    $normalNinja -match 'build [^\r\n]*ClientAllocationDiagnostics\.cpp\.obj:') {
    throw 'Diagnostic object appears in normal build rules'
}
$artifacts = @('render_client.dll', 'lux_engine_editor_ui.dll', 'lux_engine_editor_scene_session.dll',
    'lux_engine_editor_rendering.dll', 'lux_engine_editor_tooling.dll', 'editor_scene.dll')
$records = @()
foreach ($name in $artifacts) {
    foreach ($entry in @(@($normalRoot, 'normal'), @($diagnosticRoot, 'diagnostic'))) {
        $file = Join-Path (Join-Path $entry[0] 'bin') $name
        $label = $entry[1] + '-' + $name
        $exports = & dumpbin.exe /nologo /exports $file 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect exports: $file" }
        $imports = & dumpbin.exe /nologo /imports $file 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $file" }
        $exports | Set-Content -LiteralPath (Join-Path $OutputDirectory ($label + '.exports.txt'))
        $imports | Set-Content -LiteralPath (Join-Path $OutputDirectory ($label + '.imports.txt'))
        $hasFaultExports = ($exports -join "`n") -match 'lux_er1_.*allocation|RendererTestAccess|SceneTestAccess|SceneWorkbenchDiagnostics'
        if ($entry[1] -eq 'normal' -and $hasFaultExports) { throw "Fault entry exported by normal DLL: $file" }
        if ($entry[1] -eq 'diagnostic' -and $name -ne 'lux_engine_editor_tooling.dll' -and !$hasFaultExports) {
            throw "Expected diagnostic entry absent: $file"
        }
        $pdb = [IO.Path]::ChangeExtension($file, '.pdb')
        $modules = & $PdbUtil dump --modules $pdb 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect linker PDB: $pdb" }
        $modules | Set-Content -LiteralPath (Join-Path $OutputDirectory ($label + '.pdb-modules.txt'))
        $hasFaultObject = ($modules -join "`n") -match 'EditorAllocationDiagnostics\.cpp\.obj'
        $allocationOrigins = @($modules | Where-Object {
            $_ -match '^Mod ' -and $_ -match 'EditorAllocationDiagnostics|new_scalar|new_array|delete_scalar|delete_array'
        })
        if ($entry[1] -eq 'normal' -and ($hasFaultObject -or !$allocationOrigins.Count)) {
            throw "Normal allocator linker provenance missing or contaminated: $pdb"
        }
        if ($entry[1] -eq 'diagnostic' -and $name -notin @('lux_engine_editor_tooling.dll', 'editor_scene.dll') -and !$hasFaultObject) {
            throw "Diagnostic allocation replacement not linked into tested DLL: $pdb"
        }
        $records += [pscustomobject]@{
            kind = $entry[1]; path = $file; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            fault_exports = $hasFaultExports
            pdb_sha256 = (Get-FileHash -LiteralPath $pdb -Algorithm SHA256).Hash
            allocation_linker_origins = $allocationOrigins
            fault_object = $hasFaultObject
            # An empty import list is not proof of absence: the CRT can supply these functions statically.
            allocation_imports = @($imports | Where-Object { $_ -match '\?\?[23]@' } | ForEach-Object { $_.Trim() })
        }
    }
}
$records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'dll-isolation.json')
Write-Output 'Diagnostic isolation: separate caches; normal CRT allocation provenance; diagnostic replacement objects; DLL/PDB identities recorded'
