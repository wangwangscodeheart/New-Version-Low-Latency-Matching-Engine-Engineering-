param(
    [string]$Repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path,
    [string]$MockExecutable = 'build/allocation-capture-agent-tests/mock.exe'
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath $Repository).Path
$scriptPath = Join-Path $repository 'scripts/run_allocation_probe_windows.ps1'
$binaryPath = if ([IO.Path]::IsPathRooted($MockExecutable)) { $MockExecutable } else { Join-Path $repository $MockExecutable }
$root = Join-Path $repository ('build/allocation-capture-agent-tests/results-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$priorMode = [Environment]::GetEnvironmentVariable('ALLOCATION_CAPTURE_MOCK_MODE', 'Process')
$modes = @('valid', 'bad_json', 'missing_json', 'duplicate_json', 'string_number', 'uint64_overflow', 'wrong_workload', 'duplicate_row', 'duplicate_header', 'missing_row', 'invalid_ratio', 'native_failure', 'timeout')
$cases = 0
function Invoke-Collector([string]$mode, [string]$destination) {
    $env:ALLOCATION_CAPTURE_MOCK_MODE = $mode
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = 'powershell.exe'
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $scriptPath + '" -Executable "' + $binaryPath + '" -OutputDirectory "' + $destination + '" -Runs 1 -TimeoutSeconds 1'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Cannot start collector fixture child' }
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'Collector fixture watchdog timed out' }
    $output = $outputTask.GetAwaiter().GetResult()
    $errorText = $errorTask.GetAwaiter().GetResult()
    $code = $process.ExitCode
    $process.Dispose()
    [IO.File]::WriteAllText((Join-Path $root ($mode + '-collector-stdout.txt')), $output)
    [IO.File]::WriteAllText((Join-Path $root ($mode + '-collector-stderr.txt')), $errorText)
    return $code
}
try {
    foreach ($mode in $modes) {
        $destination = Join-Path $root $mode
        $code = Invoke-Collector $mode $destination
        $metadata = Get-Content -LiteralPath (Join-Path $destination 'environment.json') -Raw -Encoding utf8 | ConvertFrom-Json
        $wanted = if ($mode -eq 'valid') { 'complete' } else { 'failed' }
        if ($metadata.capture_status -cne $wanted -or (($code -eq 0) -ne ($mode -eq 'valid'))) { throw ('Unexpected collector state: ' + $mode + ' exit=' + $code) }
        foreach ($name in @('raw_run_01.txt', 'raw_run_01.stderr.txt', 'allocation_summary.csv', 'checksums.sha256')) {
            if (-not (Test-Path -LiteralPath (Join-Path $destination $name) -PathType Leaf)) { throw ('Missing evidence in ' + $mode + ': ' + $name) }
        }
        if ($mode -eq 'timeout') {
            if (-not $metadata.runs[0].timed_out) { throw 'Timeout status missing' }
            if (-not (Get-Content -LiteralPath (Join-Path $destination 'raw_run_01.txt') -Raw).Contains('partial-stdout')) { throw 'Timeout stdout lost' }
            if (-not (Get-Content -LiteralPath (Join-Path $destination 'raw_run_01.stderr.txt') -Raw).Contains('partial-stderr')) { throw 'Timeout stderr lost' }
        } elseif (-not (Get-Content -LiteralPath (Join-Path $destination 'raw_run_01.stderr.txt') -Raw).Contains('fixture-stderr-retained')) { throw ('Failure stderr lost in ' + $mode) }
        foreach ($line in Get-Content -LiteralPath (Join-Path $destination 'checksums.sha256')) {
            $parts = $line -split '\s+', 2
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $destination $parts[1])).Hash.ToLowerInvariant()
            if ($actual -cne $parts[0]) { throw ('Invalid preserved checksum: ' + $mode) }
        }
        Write-Output ($mode + ': exit=' + $code + ', capture_status=' + $metadata.capture_status + ', raw files and checksums retained')
        $cases++
    }
    $validDestination = Join-Path $root 'valid'
    $before = (Get-FileHash -LiteralPath (Join-Path $validDestination 'environment.json')).Hash
    $overwriteCode = Invoke-Collector 'valid' $validDestination
    $after = (Get-FileHash -LiteralPath (Join-Path $validDestination 'environment.json')).Hash
    if ($overwriteCode -eq 0 -or $before -cne $after) { throw 'Existing evidence overwrite guard failed' }
    $cases++
    Write-Output ('PASS collector contract mocks: ' + $cases + ' cases. These are synthetic contract fixtures, not allocation or performance measurements.')
    Write-Output ('fixture_outputs=' + $root)
} finally {
    [Environment]::SetEnvironmentVariable('ALLOCATION_CAPTURE_MOCK_MODE', $priorMode, 'Process')
}
