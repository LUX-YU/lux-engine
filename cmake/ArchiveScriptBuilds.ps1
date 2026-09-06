param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$ArchiveRoot,
    [switch]$ApplyDelete
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$plan = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$build_root = [IO.Path]::GetFullPath($plan.build_root).TrimEnd('\','/')
$archive_root = [IO.Path]::GetFullPath($ArchiveRoot).TrimEnd('\','/')
if ($archive_root.StartsWith($build_root + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Archives must live outside the build root'
}
if (Test-Path -LiteralPath $archive_root) { throw 'Use a fresh archive root; archives are never overwritten' }
$rebuildable = @('.obj','.pdb','.pch','.ilk','.idb','.iobj','.ipdb','.exp','.dll','.exe','.lib','.o','.a','.so','.dylib')
$results = [Collections.Generic.List[object]]::new()
$before_free = (Get-PSDrive -Name E).Free
New-Item -ItemType Directory -Path $archive_root | Out-Null

function getFiles([string]$path) {
    $directory = [IO.DirectoryInfo]::new($path)
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Root is a reparse point' }
    $pending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $files = [Collections.Generic.List[IO.FileInfo]]::new()
    $pending.Push($directory)
    while ($pending.Count -ne 0) {
        foreach ($entry in $pending.Pop().EnumerateFileSystemInfos()) {
            if ($entry.Name -eq '.git') { throw 'Source Git worktree found; refuse deletion' }
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Nested reparse point found' }
            if ($entry -is [IO.DirectoryInfo]) { $pending.Push($entry) } else { $files.Add($entry) }
        }
    }
    return @($files | Sort-Object FullName)
}
function stamp($files) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($file in $files) {
            $record = [Text.Encoding]::UTF8.GetBytes("$($file.FullName)|$($file.Length)|$($file.LastWriteTimeUtc.Ticks)`n")
            $null = $sha.TransformBlock($record,0,$record.Length,$record,0)
        }
        $null = $sha.TransformFinalBlock([byte[]]::new(0),0,0)
        return [Convert]::ToHexString($sha.Hash)
    } finally { $sha.Dispose() }
}
function persistResults {
    $results | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath "$archive_root/results.json" -Encoding utf8
}

foreach ($planned in $plan.entries) {
    $result = [ordered]@{path=$planned.path;status='CHECKING'}
    try {
        $path = (Resolve-Path -LiteralPath $planned.path).Path.TrimEnd('\','/')
        $directory = [IO.DirectoryInfo]::new($path)
        $configuration = $directory.Parent
        if ($configuration.Parent.FullName -ne $build_root -or
            $configuration.Name -notin @('Debug','Release','RelWithDebInfo') -or
            $directory.Name -in $plan.retained_names) { throw 'Path is outside approved leaf scope or explicitly retained' }
        $active = Get-CimInstance Win32_Process | Where-Object {
            ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($path + '\',[StringComparison]::OrdinalIgnoreCase)) -or
            ($_.Name -in @('cmake.exe','ctest.exe','ninja.exe','cl.exe') -and $_.CommandLine -and
                ($_.CommandLine.IndexOf($path,[StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                 $_.CommandLine.IndexOf($path.Replace('\','/'),[StringComparison]::OrdinalIgnoreCase) -ge 0))
        }
        if ($active) { throw 'Active process references this root' }
        $files = getFiles $path
        $bytes = ($files | Measure-Object Length -Sum).Sum
        if ($files.Count -ne $planned.files -or $bytes -ne $planned.bytes) { throw 'Root changed since approved inventory' }
        $homes = @($files | Where-Object Name -eq 'CMakeCache.txt' | ForEach-Object {
            $match = Select-String -LiteralPath $_.FullName -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)' | Select-Object -First 1
            if ($match) { $match.Matches[0].Groups[1].Value }
        } | Sort-Object -Unique)
        if (Compare-Object -ReferenceObject @($planned.homes) -DifferenceObject $homes) { throw 'CMake source owner changed' }
        $result.bytes = $bytes
        $result.files = $files.Count
        $result.source_owners = $homes
        $original_stamp = stamp $files
        if (!$ApplyDelete) {
            $result.status = 'PREVIEW_VALIDATED'
        } else {
            $name = "$($configuration.Name)__$($directory.Name)"
            $archive = Join-Path $archive_root ($name + '.zip')
            $members = [Collections.Generic.List[object]]::new()
            $zip = [IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
            try {
                foreach ($file in $files) {
                    if ($file.Extension.ToLowerInvariant() -in $rebuildable) { continue }
                    $relative = $file.FullName.Substring($path.Length + 1).Replace('\','/')
                    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
                    $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                        $zip,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal)
                    $members.Add([pscustomobject]@{path=$relative;bytes=$file.Length;sha256=$hash})
                }
            } finally { $zip.Dispose() }
            $zip = [IO.Compression.ZipFile]::OpenRead($archive)
            try {
                if ($zip.Entries.Count -ne $members.Count) { throw 'Archive member count mismatch' }
                foreach ($member in $members) {
                    $entry = $zip.GetEntry($member.path)
                    if ($null -eq $entry -or $entry.Length -ne $member.bytes) { throw 'Archive member length mismatch' }
                    $stream = $entry.Open()
                    $sha = [Security.Cryptography.SHA256]::Create()
                    try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)) }
                    finally { $sha.Dispose(); $stream.Dispose() }
                    if ($hash -ne $member.sha256) { throw 'Archive content hash mismatch' }
                }
            } finally { $zip.Dispose() }
            $members | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$archive.members.json" -Encoding utf8
            $result.archive = $archive
            $result.archive_sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
            $result.archive_bytes = (Get-Item -LiteralPath $archive).Length
            $result.archived_files = $members.Count
            if ((stamp (getFiles $path)) -ne $original_stamp) { throw 'Build root changed during archival' }
            $result.status = 'ARCHIVE_VERIFIED'
            $results.Add($result)
            persistResults
            # Exact, validated leaf; source roots, configuration roots and broad globs are never removed.
            $result.deletion_started = $true
            Remove-Item -LiteralPath $path -Recurse -Force
            $result.status = 'DELETED_AFTER_VERIFIED_ARCHIVE'
            persistResults
            if ($results.Count % 10 -eq 0) { Write-Output "[cleanup] $($results.Count) roots processed" }
            continue
        }
    } catch {
        $result.status = if ($result.deletion_started) { 'PARTIAL_DELETE_ERROR' } else { 'RETAINED_ERROR' }
        $result.error = $_.Exception.Message
    }
    if (!$results.Contains($result)) { $results.Add($result) }
    persistResults
}
$after_free = (Get-PSDrive -Name E).Free
Write-Output "[cleanup] roots=$($results.Count) deleted=$(@($results | Where-Object status -eq 'DELETED_AFTER_VERIFIED_ARCHIVE').Count) free_delta_bytes=$($after_free-$before_free)"
