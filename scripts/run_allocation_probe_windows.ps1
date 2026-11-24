param(
    [string]$Executable = "build/public-feed-alloc-release/matching_engine_public_feed_allocations.exe",
    [string]$OutputDirectory = "benchmarks/results/2026-11-24-public-feed-allocations",
    [int]$Runs = 3,
    [ValidateRange(1, 60)][int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
if ($Runs -le 0) {
    throw "Runs must be a positive integer"
}

function Read-UInt64([string]$Text, [string]$Field) {
    [uint64]$parsed = 0
    if ($Text -cnotmatch '^[0-9]+$' -or
        -not [uint64]::TryParse($Text, [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        throw "UInt64 integer required for $Field"
    }
    return $parsed
}

function Read-MetadataUInt64($Value, [string]$Field) {
    # JSON strings/booleans are not numeric metadata. Avoid a permissive
    # PowerShell comparison coercing a claimed string or boolean to an integer.
    if ($null -eq $Value -or -not ($Value -is [ValueType]) -or $Value -is [bool]) {
        throw "Numeric JSON integer required for $Field"
    }
    return Read-UInt64 ([Convert]::ToString($Value, [Globalization.CultureInfo]::InvariantCulture)) $Field
}

function Multiply-UInt64([uint64]$Value, [uint64]$Factor, [string]$Field) {
    # These factors are at most1284, so Decimal represents the entire product
    # exactly before a UInt64 conversion; no double/rounded division boundary.
    [decimal]$product = [decimal]$Value * [decimal]$Factor
    if ($product -gt [decimal][uint64]::MaxValue) { throw "Workload UInt64 overflow for $Field" }
    return [uint64]$product
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$binaryPath = if ([IO.Path]::IsPathRooted($Executable)) { $Executable } else { Join-Path $repo $Executable }
$binary = (Resolve-Path -LiteralPath $binaryPath).Path
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Allocation probe executable is not a file: $binary"
}
$destination = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
}
if (Test-Path -LiteralPath $destination) {
    throw "Refusing to overwrite existing allocation evidence: $destination"
}
New-Item -ItemType Directory -Path $destination | Out-Null

$allocationHeader = "scenario,phase,commands,public_messages,wire_bytes,allocation_calls,requested_bytes,aligned_calls,zero_size_calls,overflowed,thread_scope,counter_scope,timing_reported"
$summary = New-Object 'System.Collections.Generic.List[string]'
$summary.Add("run," + $allocationHeader)
$runRecords = New-Object 'System.Collections.Generic.List[object]'
$workDirectories = @(for ($run = 1; $run -le $Runs; ++$run) {
    "build/allocation_probe_work/run_{0:D2}_{1}" -f $run, [Guid]::NewGuid().ToString("N")
})
$environment = [ordered]@{
    schema_version = 1
    capture_kind = "public_feed_cpp_new_allocation_probe"
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    executable = $Executable.Replace("\", "/")
    executable_resolved_path = $binary
    invocation_working_directory = (Get-Location).Path
    benchmark_working_directories = $workDirectories
    benchmark_working_directory_policy = "unique_private_directory_per_run_under_build_retained"
    raw_output_policy = "stdout_and_stderr_captured_unconditionally_in_separate_files"
    raw_output_environment = "MATCHING_ENGINE_RAW_OUTPUT is not required and is inherited unchanged"
    requested_runs = $Runs
    per_process_timeout_seconds = $TimeoutSeconds
    affinity_frozen = $false
    power_plan_frozen = $false
    cpu_frequency_frozen = $false
    cpu_or_os_settings_changed = $false
    timing_reported = $false
    capture_status = "in_progress"
    runs = $runRecords
}

try {
    $commitLines = @(& git -C $repo rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw "Unable to read repository commit" }
    $status = @(& git -c core.quotepath=false -C $repo status --porcelain=v1 --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw "Unable to read repository worktree status" }
    $trackedChanges = @($status | Where-Object { -not $_.StartsWith("?? ") })
    $untrackedChanges = @($status | Where-Object { $_.StartsWith("?? ") })
    $environment["git_commit"] = ($commitLines -join "").Trim()
    $environment["tracked_worktree_clean"] = ($trackedChanges.Count -eq 0)
    $environment["current_worktree_dirty"] = ($status.Count -ne 0)
    $environment["tracked_changed_files"] = $trackedChanges
    $environment["untracked_files"] = $untrackedChanges
    $environment["executable_sha256"] = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash.ToLowerInvariant()
    $environment["capture_script"] = "scripts/run_allocation_probe_windows.ps1"
    $environment["capture_script_sha256"] = (Get-FileHash -Algorithm SHA256 -LiteralPath $PSCommandPath).Hash.ToLowerInvariant()
    $environment["operating_system"] = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
    $environment["os_version"] = [Environment]::OSVersion.VersionString
    $environment["cpu"] = if ($env:PROCESSOR_IDENTIFIER) { $env:PROCESSOR_IDENTIFIER } else { "unknown" }
    $environment["logical_processors"] = [Environment]::ProcessorCount
    $powerCommand = Get-Command powercfg -ErrorAction SilentlyContinue
    if ($null -ne $powerCommand) {
        $powerLines = @(& $powerCommand.Source /getactivescheme 2>&1)
        $environment["power_plan"] = ($powerLines -join "`n").Trim()
        $environment["power_plan_query_exit_code"] = $LASTEXITCODE
    } else {
        $environment["power_plan"] = "unavailable (powercfg not found)"
    }
    & git -C $repo check-ignore --quiet -- "build/allocation_probe_work/"
    if ($LASTEXITCODE -ne 0) {
        throw "Private allocation probe working directories must be ignored by git"
    }

    $expectedScenarios = @("journal_public_feed_sync", "full_v2_public_feed_sync", "full_v2_public_feed_burst16")
    $expectedPhases = @("setup", "run", "stop_and_validation", "teardown")
    $numericFields = @("commands", "public_messages", "wire_bytes", "allocation_calls", "requested_bytes", "aligned_calls", "zero_size_calls")
    for ($run = 1; $run -le $Runs; ++$run) {
        $rawPath = Join-Path $destination ("raw_run_{0:D2}.txt" -f $run)
        $stderrPath = Join-Path $destination ("raw_run_{0:D2}.stderr.txt" -f $run)
        $workDirectory = Join-Path $repo $workDirectories[$run - 1]
        New-Item -ItemType Directory -Path $workDirectory -Force | Out-Null
        $record = [ordered]@{
            run = $run
            started_at_utc = [DateTime]::UtcNow.ToString("o")
            working_directory = $workDirectories[$run - 1]
            stdout_file = [IO.Path]::GetFileName($rawPath)
            stderr_file = [IO.Path]::GetFileName($stderrPath)
            allocation_rows = 0
            timed_out = $false
            stdout_capture_complete = $false
            stderr_capture_complete = $false
            validation_passed = $false
        }
        $runRecords.Add($record)
        $stdout = ""
        $stderr = ""
        $stdoutTask = $null
        $stderrTask = $null
        $started = $false
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
        $process.StartInfo.FileName = $binary
        $process.StartInfo.WorkingDirectory = $workDirectory
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        try {
            if (-not $process.Start()) { throw "Unable to start allocation probe run $run" }
            $started = $true
            # Read both pipes concurrently so a full stderr pipe cannot deadlock stdout capture.
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                $record["timed_out"] = $true
                # Only this exact child, which this invocation created/owns.
                $process.Kill()
                if (-not $process.WaitForExit(5000)) { throw "Allocation probe child did not exit after timeout termination" }
                throw "Allocation probe run $run exceeded $TimeoutSeconds seconds (raw output preserved)"
            }
            $record["exit_code"] = $process.ExitCode
            if (-not $stdoutTask.Wait(5000) -or -not $stderrTask.Wait(5000)) {
                throw "Allocation probe run $run output capture did not finish (result rejected)"
            }
            $stdout = $stdoutTask.GetAwaiter().GetResult()
            $stderr = $stderrTask.GetAwaiter().GetResult()
        } finally {
            # Retain raw evidence even when process startup, capture, or validation fails.
            try {
                if ($started -and -not $process.HasExited) {
                    $process.Kill()
                    if (-not $process.WaitForExit(5000)) { throw "Owned allocation probe child did not terminate" }
                }
                if ($started -and $process.HasExited) { $record["exit_code"] = $process.ExitCode }
                if ($null -ne $stdoutTask -and -not $stdoutTask.IsCompleted) { $null = $stdoutTask.Wait(5000) }
                if ($null -ne $stderrTask -and -not $stderrTask.IsCompleted) { $null = $stderrTask.Wait(5000) }
                if ($null -ne $stdoutTask -and $stdoutTask.IsCompleted) {
                    $stdout = $stdoutTask.GetAwaiter().GetResult()
                    $record["stdout_capture_complete"] = $true
                }
                if ($null -ne $stderrTask -and $stderrTask.IsCompleted) {
                    $stderr = $stderrTask.GetAwaiter().GetResult()
                    $record["stderr_capture_complete"] = $true
                }
            } finally {
                [IO.File]::WriteAllText($rawPath, $stdout, (New-Object Text.UTF8Encoding($false)))
                [IO.File]::WriteAllText($stderrPath, $stderr, (New-Object Text.UTF8Encoding($false)))
                $record["finished_at_utc"] = [DateTime]::UtcNow.ToString("o")
                $process.Dispose()
            }
        }

        $headerCount = 0
        $validationErrors = New-Object 'System.Collections.Generic.List[string]'
        $pairs = New-Object 'System.Collections.Generic.HashSet[string]'
        $metadata = New-Object 'System.Collections.Generic.List[string]'
        $numericRows = New-Object 'System.Collections.Generic.List[object]'
        foreach ($line in ($stdout -split "\r?\n")) {
            if ($line.StartsWith("METADATA_JSON")) { $metadata.Add($line) }
            if (-not $line.StartsWith("ALLOCATION_CSV,")) { continue }
            $csv = $line.Substring("ALLOCATION_CSV,".Length)
            if ($csv.StartsWith("scenario,")) {
                ++$headerCount
                if ($csv -cne $allocationHeader) { $validationErrors.Add("unexpected ALLOCATION_CSV header") }
                continue
            }
            # Store every advertised data row before validation for partial/failure evidence.
            $summary.Add("$run," + $csv)
            ++$record["allocation_rows"]
            if (($csv -split ",").Count -ne 13) {
                $validationErrors.Add("allocation row has an unexpected field count")
                continue
            }
            $row = ConvertFrom-Csv -InputObject @($allocationHeader, $csv)
            if ($expectedScenarios -cnotcontains $row.scenario -or $expectedPhases -cnotcontains $row.phase) {
                $validationErrors.Add("unexpected scenario/phase: $($row.scenario)/$($row.phase)")
            }
            if (-not $pairs.Add($row.scenario + "/" + $row.phase)) {
                $validationErrors.Add("duplicate scenario/phase: $($row.scenario)/$($row.phase)")
            }
            $numbers = @{}
            foreach ($field in $numericFields) {
                try { $numbers[$field] = Read-UInt64 $row.$field $field }
                catch { $validationErrors.Add($_.Exception.Message) }
            }
            if ($numbers.Count -eq $numericFields.Count) {
                if ($numbers.aligned_calls -gt $numbers.allocation_calls -or
                    $numbers.zero_size_calls -gt $numbers.allocation_calls) {
                    $validationErrors.Add("aligned/zero-size calls cannot exceed allocation calls")
                }
                $numericRows.Add($numbers)
            }
            if ($row.overflowed -cne "0") { $validationErrors.Add("allocation counter overflowed or invalid overflow flag") }
            if ($row.thread_scope -cne "calling_thread_only") { $validationErrors.Add("unexpected thread_scope") }
            if ($row.counter_scope -cne "successful_cpp_new_requests") { $validationErrors.Add("unexpected counter_scope") }
            if ($row.timing_reported -cne "false") { $validationErrors.Add("allocation probe must not report timing") }
        }
        $record["metadata_json_lines"] = @($metadata.ToArray())
        if ($metadata.Count -ne 1) {
            $validationErrors.Add("exactly one METADATA_JSON line required")
        } else {
            try {
                $prefix = "METADATA_JSON,"
                if (-not $metadata[0].StartsWith($prefix)) { throw "invalid METADATA_JSON prefix" }
                $jsonText = $metadata[0].Substring($prefix.Length)
                if (-not $jsonText.TrimStart().StartsWith('{')) { throw "METADATA_JSON must be one JSON object" }
                $probeMetadata = ConvertFrom-Json -InputObject $jsonText
                if ((Read-MetadataUInt64 $probeMetadata.schema_version 'schema_version') -ne 1) { throw "unsupported probe metadata schema" }
                if ($probeMetadata.measurement_scope -cne "successful_cpp_new_requests" -or
                    $probeMetadata.counter_scope -cne "successful_cpp_new_requests" -or
                    $probeMetadata.thread_scope -cne "calling_thread_only") { throw "invalid allocation measurement scope" }
                foreach ($flag in @('worker_threads_excluded', 'total_heap_not_measured', 'setup_includes_fixture_owner')) {
                    if (-not ($probeMetadata.$flag -is [bool]) -or $probeMetadata.$flag -ne $true) { throw "metadata $flag must be true" }
                }
                foreach ($flag in @('timing_reported', 'latency_samples_collected')) {
                    if (-not ($probeMetadata.$flag -is [bool]) -or $probeMetadata.$flag -ne $false) { throw "metadata $flag must be false" }
                }
                if ($probeMetadata.workload -cne "synthetic_mixed_visible_v1" -or
                    $probeMetadata.workload_counters -cne "full_scenario_context_repeated_per_phase") { throw "unexpected workload/counter interpretation" }
                if ($probeMetadata.build_type -cne "Release") { throw "allocation evidence requires a Release probe" }
                if ((Read-MetadataUInt64 $probeMetadata.scenarios 'scenarios') -ne 3 -or
                    (Read-MetadataUInt64 $probeMetadata.phases 'phases') -ne 4) { throw "expected three scenarios and four phases" }
                $cycles = Read-MetadataUInt64 $probeMetadata.cycles_per_symbol 'cycles_per_symbol'
                if ($cycles -eq 0) { throw "cycles_per_symbol must be positive" }
                $expectedWorkload = @{
                    commands = (Multiply-UInt64 $cycles 24 'commands')
                    public_messages = (Multiply-UInt64 $cycles 27 'public_messages')
                    wire_bytes = (Multiply-UInt64 $cycles 1284 'wire_bytes')
                }
                foreach ($numbers in $numericRows) {
                    foreach ($field in @('commands', 'public_messages', 'wire_bytes')) {
                        if ($numbers[$field] -ne $expectedWorkload[$field]) { $validationErrors.Add("workload $field does not match cycles_per_symbol") }
                    }
                }
                $record["probe_metadata"] = $probeMetadata
            } catch { $validationErrors.Add("invalid METADATA_JSON: " + $_.Exception.Message) }
        }
        if ($headerCount -ne 1) { $validationErrors.Add("exactly one ALLOCATION_CSV header required") }
        if ($record["allocation_rows"] -ne ($expectedScenarios.Count * $expectedPhases.Count)) {
            $validationErrors.Add("expected 12 allocation rows (all scenario/phase pairs)")
        }
        foreach ($scenario in $expectedScenarios) {
            foreach ($phase in $expectedPhases) {
                if (-not $pairs.Contains($scenario + "/" + $phase)) {
                    $validationErrors.Add("missing scenario/phase: $scenario/$phase")
                }
            }
        }
        $record["validation_errors"] = @($validationErrors.ToArray())
        if ($record["exit_code"] -ne 0) {
            throw "Allocation probe run $run failed with exit code $($record['exit_code']) (raw output and partial summary preserved)"
        }
        if ($validationErrors.Count -gt 0) {
            throw "Allocation probe run $run failed validation: $($validationErrors -join '; ') (raw output and partial summary preserved)"
        }
        $record["validation_passed"] = $true
    }
    $environment["capture_status"] = "complete"
} catch {
    $environment["capture_status"] = "failed"
    $environment["capture_error"] = $_.Exception.Message
    throw
} finally {
    $environment["finished_at_utc"] = [DateTime]::UtcNow.ToString("o")
    $environment["completed_runs"] = @($runRecords | Where-Object { $_["validation_passed"] }).Count
    $summary | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $destination "allocation_summary.csv")
    $environment | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $destination "environment.json")
    $hashes = Get-ChildItem -LiteralPath $destination -File | Sort-Object Name | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    }
    $hashes | Set-Content -Encoding ASCII -LiteralPath (Join-Path $destination "checksums.sha256")
}
Write-Output "Allocation probe evidence written to $destination"
