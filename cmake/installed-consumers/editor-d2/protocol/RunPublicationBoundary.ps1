param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Boundary,
    [switch]$Conflict
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $Root -Force | Out-Null
$caseRoot = Join-Path $Root ($Boundary.Replace(':','-') + '-' + [guid]::NewGuid().ToString('N'))
$log = $caseRoot + '.interrupt.log'
$previous = $env:LUX_EDITOR_PUBLICATION_INTERRUPT
try {
    $env:LUX_EDITOR_PUBLICATION_INTERRUPT = $Boundary
    & $Executable $caseRoot interrupt *> $log
    $result = $LASTEXITCODE
} finally {
    $env:LUX_EDITOR_PUBLICATION_INTERRUPT = $previous
}
Get-Content -LiteralPath $log
if ($result -ne 86) { throw "Expected deliberate boundary exit 86, got $result" }
$marker = 'DELIBERATE PUBLICATION INTERRUPTION boundary=' + $Boundary + ' exit=86'
if (!(Select-String -LiteralPath $log -SimpleMatch $marker -Quiet)) { throw "Missing exact boundary marker: $Boundary" }
$inventory = Get-ChildItem -LiteralPath $caseRoot -File -Force -Recurse | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($caseRoot, $_.FullName); bytes = $_.Length;
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
}
$inventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath ($caseRoot + '.interrupted-files.json')
$mode = if ($Conflict) { 'conflict' } else { 'recover' }
$recoveryLog = $caseRoot + '.recovery.log'
& $Executable $caseRoot $mode $Boundary *> $recoveryLog
$result = $LASTEXITCODE
Get-Content -LiteralPath $recoveryLog
if ($result -ne 0 -or !(Select-String -LiteralPath $recoveryLog -Pattern '^PASS ' -Quiet)) {
    throw "New-process $mode failed: exit=$result boundary=$Boundary"
}
