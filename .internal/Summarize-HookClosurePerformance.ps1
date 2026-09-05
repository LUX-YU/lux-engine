param([string]$Root='E:/SyncForder/CodeRepos/build/RelWithDebInfo/hook-closure-performance')
$ErrorActionPreference='Stop'
function Percentile([double[]]$Sorted, [double]$Fraction) {
    $rank = ($Sorted.Count-1) * $Fraction
    $left = [int][Math]::Floor($rank)
    $right = [int][Math]::Ceiling($rank)
    return $Sorted[$left] + ($Sorted[$right]-$Sorted[$left])*($rank-$left)
}
$summary = foreach($file in Get-ChildItem -LiteralPath $Root -Filter '*.csv' -File) {
    if($file.Name -notmatch '^(B[012])-(.+)-(\d+)-r(\d+)\.csv$'){continue}
    $point=$matches[1]; $scenario=$matches[2]; $population=[int]$matches[3]; $round=[int]$matches[4]
    $rows=@(Import-Csv -LiteralPath $file.FullName)
    if($scenario.StartsWith('scene-lua-')){$rows=@($rows | Select-Object -Skip 95)}
    if($rows.Count -eq 0){throw "Empty measurement: $($file.Name)"}
    [double[]]$values=@($rows | ForEach-Object {[double]$_.nanoseconds} | Sort-Object)
    $mean=($values | Measure-Object -Average).Average
    $variance=0.0
    foreach($value in $values){$variance+=($value-$mean)*($value-$mean)}
    $last=$rows[-1]
    [pscustomobject][ordered]@{
        point=$point; scenario=$scenario; size=$population; round=$round; samples=$rows.Count;
        git_commit=$last.git_commit; p50_ms=(Percentile $values 0.5)/1e6;
        p90_ms=(Percentile $values 0.9)/1e6; p95_ms=(Percentile $values 0.95)/1e6;
        p99_ms=(Percentile $values 0.99)/1e6; max_ms=$values[-1]/1e6; mean_ms=$mean/1e6;
        stddev_ms=[Math]::Sqrt($variance/$values.Count)/1e6;
        ns_per_object=(Percentile $values 0.5)/$population;
        max_engine_allocations=($rows | ForEach-Object {[long]$_.allocations} | Measure-Object -Maximum).Maximum;
        final_checksum=$last.checksum; final_physics_queries=$last.physics_queries;
        active_instances=$last.active_instances; final_resumes=$last.resumes;
        source_csv=$file.FullName
    }
}
$summary | Sort-Object scenario,size,point,round | Export-Csv -NoTypeInformation -Encoding utf8 "$Root/summary.csv"
$aggregates = foreach($group in $summary | Group-Object scenario,size,point) {
    $first=$group.Group[0]
    [double[]]$medians=@($group.Group.p50_ms | Sort-Object)
    [double[]]$tails=@($group.Group.p95_ms | Sort-Object)
    [pscustomobject]@{
        scenario=$first.scenario; size=$first.size; point=$first.point; independent_processes=$group.Count;
        median_process_p50_ms=Percentile $medians 0.5; min_process_p50_ms=$medians[0]; max_process_p50_ms=$medians[-1];
        median_process_p95_ms=Percentile $tails 0.5; min_process_p95_ms=$tails[0]; max_process_p95_ms=$tails[-1]
    }
}
$aggregates | Sort-Object scenario,size,point | Export-Csv -NoTypeInformation -Encoding utf8 "$Root/aggregate.csv"
$aggregates | Sort-Object scenario,size,point | Format-Table -AutoSize
