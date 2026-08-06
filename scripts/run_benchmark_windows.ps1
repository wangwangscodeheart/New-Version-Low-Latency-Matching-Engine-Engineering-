param(
    [string]$Executable = "build/v2_ninja/matching_engine_v2_benchmarks.exe",
    [string]$OutputDirectory = "benchmarks/results/2026-08-07-v2-baseline",
    [int]$Runs = 5
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$binary = (Resolve-Path (Join-Path $repo $Executable)).Path
$destination = Join-Path $repo $OutputDirectory
if (Test-Path $destination) {
    throw "Refusing to overwrite existing benchmark evidence: $destination"
}
New-Item -ItemType Directory -Path $destination | Out-Null

$commit = (& git -C $repo rev-parse HEAD).Trim()
$status = & git -C $repo status --porcelain --untracked-files=no
$cpu = if ($env:PROCESSOR_IDENTIFIER) { $env:PROCESSOR_IDENTIFIER } else { "unknown" }
$environment = [ordered]@{
    schema_version = 1
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    git_commit = $commit
    tracked_worktree_clean = ($status.Count -eq 0)
    executable = $Executable.Replace("\", "/")
    operating_system = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
    os_version = [Environment]::OSVersion.VersionString
    cpu = $cpu
    logical_processors = [Environment]::ProcessorCount
    power_plan = (& powercfg /getactivescheme).Trim()
    requested_runs = $Runs
}
$environment | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $destination "environment.json")

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("run,scenario,operations,batch_avg_ns,throughput_per_sec,sample_median_ns,sample_p99_ns,sample_p999_ns,sample_max_ns,checksum")
for ($run = 1; $run -le $Runs; ++$run) {
    $rawPath = Join-Path $destination ("raw_run_{0:D2}.txt" -f $run)
    $lines = & $binary 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Benchmark run $run failed" }
    $lines | Set-Content -Encoding UTF8 $rawPath
    foreach ($line in $lines) {
        $text = [string]$line
        if ($text.StartsWith("RESULT_CSV,") -and
            -not $text.StartsWith("RESULT_CSV,scenario,")) {
            $summary.Add("$run," + $text.Substring(11))
        }
    }
}
$summary | Set-Content -Encoding UTF8 (Join-Path $destination "summary.csv")
$hashes = Get-ChildItem -File $destination | Sort-Object Name | ForEach-Object {
    $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
    "$hash  $($_.Name)"
}
$hashes | Set-Content -Encoding ASCII (Join-Path $destination "checksums.sha256")
Write-Output "Benchmark evidence written to $destination"
