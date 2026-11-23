param(
    [string]$Executable = "build/v2_ninja/matching_engine_v2_benchmarks.exe",
    [string]$OutputDirectory = "benchmarks/results/2026-08-07-v2-baseline",
    [int]$Runs = 5
)

$ErrorActionPreference = "Stop"
if ($Runs -le 0) {
    throw "Runs must be a positive integer"
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$binary = (Resolve-Path (Join-Path $repo $Executable)).Path
$destination = Join-Path $repo $OutputDirectory
if (Test-Path $destination) {
    throw "Refusing to overwrite existing benchmark evidence: $destination"
}
New-Item -ItemType Directory -Path $destination | Out-Null

$commit = (& git -C $repo rev-parse HEAD).Trim()
$status = @(& git -c core.quotepath=false -C $repo status --porcelain=v1 --untracked-files=all)
$trackedChanges = @($status | Where-Object { -not $_.StartsWith("?? ") })
$untrackedChanges = @($status | Where-Object { $_.StartsWith("?? ") })
$cpu = if ($env:PROCESSOR_IDENTIFIER) { $env:PROCESSOR_IDENTIFIER } else { "unknown" }
$workRoot = Join-Path $repo "build/benchmark_work"
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
$workDirectories = @(for ($run = 1; $run -le $Runs; ++$run) {
    "build/benchmark_work/run_{0:D2}_{1}" -f $run, [Guid]::NewGuid().ToString("N")
})
$environment = [ordered]@{
    schema_version = 2
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    git_commit = $commit
    tracked_worktree_clean = ($trackedChanges.Count -eq 0)
    current_worktree_dirty = ($status.Count -ne 0)
    tracked_changed_files = $trackedChanges
    untracked_files = $untrackedChanges
    executable = $Executable.Replace("\", "/")
    executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash.ToLowerInvariant()
    invocation_working_directory = (Get-Location).Path
    benchmark_working_directories = $workDirectories
    benchmark_working_directory_policy = "unique_private_directory_per_run_under_build_retained"
    operating_system = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
    os_version = [Environment]::OSVersion.VersionString
    cpu = $cpu
    logical_processors = [Environment]::ProcessorCount
    power_plan = (& powercfg /getactivescheme).Trim()
    affinity_frozen = $false
    power_plan_frozen = $false
    cpu_frequency_frozen = $false
    cpu_or_os_settings_changed = $false
    raw_output_environment = "MATCHING_ENGINE_RAW_OUTPUT=1 (restored after capture)"
    requested_runs = $Runs
}
$environment | ConvertTo-Json -Depth 3 | Set-Content -Encoding UTF8 (Join-Path $destination "environment.json")

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("run,scenario,operations,batch_avg_ns,throughput_per_sec,sample_median_ns,sample_p99_ns,sample_p999_ns,sample_max_ns,checksum")
$publicHeader = "scenario,commands,public_messages,wire_bytes,visible_commands,no_visible_commands,peak_pending_batches,peak_pending_messages,peak_pending_bytes,latency_samples,drain_every_commands,journal_boundary,latency_boundary,allocation_measurement"
$publicSummary = New-Object System.Collections.Generic.List[string]
$publicSummary.Add("run," + $publicHeader)
$previousRawOutput = [Environment]::GetEnvironmentVariable("MATCHING_ENGINE_RAW_OUTPUT", "Process")
try {
    $env:MATCHING_ENGINE_RAW_OUTPUT = "1"
    for ($run = 1; $run -le $Runs; ++$run) {
        $rawPath = Join-Path $destination ("raw_run_{0:D2}.txt" -f $run)
        $workDirectory = Join-Path $repo $workDirectories[$run - 1]
        New-Item -ItemType Directory -Path $workDirectory | Out-Null
        Push-Location -LiteralPath $workDirectory
        $captureErrorPreference = $ErrorActionPreference
        try {
            # Windows PowerShell otherwise treats redirected native stderr as a
            # terminating error before the exit code and raw output can be saved.
            $ErrorActionPreference = "Continue"
            $lines = @(& $binary 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $captureErrorPreference
            Pop-Location
        }
        $lines | Set-Content -Encoding UTF8 $rawPath
        $resultRows = 0
        $publicRows = 0
        $publicHeaderSeen = $false
        foreach ($line in $lines) {
            $text = [string]$line
            if ($text.StartsWith("RESULT_CSV,") -and
                -not $text.StartsWith("RESULT_CSV,scenario,")) {
                $summary.Add("$run," + $text.Substring("RESULT_CSV,".Length))
                ++$resultRows
            } elseif ($text.StartsWith("PUBLIC_FEED_CSV,scenario,")) {
                if ($text -ne ("PUBLIC_FEED_CSV," + $publicHeader)) {
                    throw "Unexpected PUBLIC_FEED_CSV header in benchmark run $run"
                }
                $publicHeaderSeen = $true
            } elseif ($text.StartsWith("PUBLIC_FEED_CSV,")) {
                $publicSummary.Add("$run," + $text.Substring("PUBLIC_FEED_CSV,".Length))
                ++$publicRows
            }
        }
        if ($exitCode -ne 0) { throw "Benchmark run $run failed with exit code $exitCode (raw output preserved)" }
        if ($resultRows -eq 0) { throw "Benchmark run $run produced no RESULT_CSV rows" }
        if ($publicHeaderSeen -and $publicRows -eq 0) {
            throw "Benchmark run $run advertised PUBLIC_FEED_CSV but produced no public-feed rows"
        }
        if ($publicRows -gt 0 -and -not $publicHeaderSeen) {
            throw "Benchmark run $run produced PUBLIC_FEED_CSV rows without a header"
        }
    }
} finally {
    [Environment]::SetEnvironmentVariable("MATCHING_ENGINE_RAW_OUTPUT", $previousRawOutput, "Process")
    $summary | Set-Content -Encoding UTF8 (Join-Path $destination "summary.csv")
    $publicSummary | Set-Content -Encoding UTF8 (Join-Path $destination "public_feed_summary.csv")
    $hashes = Get-ChildItem -File $destination | Sort-Object Name | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    }
    $hashes | Set-Content -Encoding ASCII (Join-Path $destination "checksums.sha256")
}
Write-Output "Benchmark evidence written to $destination"
