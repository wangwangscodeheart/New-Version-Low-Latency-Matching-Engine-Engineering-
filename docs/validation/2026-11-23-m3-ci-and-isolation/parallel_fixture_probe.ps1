param(
    [Parameter(Mandatory = $true)][string]$Repository,
    [string[]]$BuildDirectories = @('build/itch-integrity-no-zlib-release', 'build/itch-integrity-release', 'build/itch-integrity-asan'),
    [ValidateRange(1, 8)][int]$Copies = 2
)
$ErrorActionPreference = 'Stop'
$repositoryPath = (Resolve-Path -LiteralPath $Repository).Path
if (-not (Test-Path -LiteralPath (Join-Path $repositoryPath 'CMakeLists.txt') -PathType Leaf)) { throw 'Not a configured project source root' }
$runParent = Join-Path $repositoryPath 'build/fixture-isolation-probe'
New-Item -ItemType Directory -Path $runParent -Force | Out-Null
$runRoot = Join-Path $runParent ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$sharedTemp = Join-Path $runRoot 'shared-temp'
New-Item -ItemType Directory -Path $sharedTemp | Out-Null
$targets = @('unit', 'snapshot', 'journal_replay', 'async_journal', 'command_journal', 'market_data_generator', 'market_data', 'order_type')
$cases = [Collections.Generic.List[object]]::new()
$failures = 0
Write-Output 'Fixture isolation probe only; NOT parallel matching or performance evidence.'
Write-Output ('shared_temp=' + $sharedTemp)
try {
    foreach ($build in $BuildDirectories) {
        if ($build -notmatch '^build/[A-Za-z0-9_-]+$') { throw 'Build directory must be a direct child of build/' }
        foreach ($target in $targets) {
            $executable = Join-Path $repositoryPath ($build + '/matching_engine_' + $target + '_tests.exe')
            if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw ('Missing executable: ' + $executable) }
            for ($copy = 0; $copy -lt $Copies; $copy++) {
                $label = (Split-Path -Leaf $build) + '-' + $target + '-' + $copy
                $working = Join-Path $runRoot $label
                New-Item -ItemType Directory -Path $working | Out-Null
                $start = [Diagnostics.ProcessStartInfo]::new()
                $start.FileName = $executable
                $start.WorkingDirectory = $working
                $start.UseShellExecute = $false
                $start.CreateNoWindow = $true
                $start.RedirectStandardOutput = $true
                $start.RedirectStandardError = $true
                foreach ($variable in @('TEMP', 'TMP', 'TMPDIR')) { $start.EnvironmentVariables[$variable] = $sharedTemp }
                $process = [Diagnostics.Process]::new()
                $process.StartInfo = $start
                if (-not $process.Start()) { $process.Dispose(); throw ('Cannot start ' + $label) }
                $cases.Add([pscustomobject]@{
                    Label = $label; Working = $working; Process = $process
                    Output = $process.StandardOutput.ReadToEndAsync()
                    Error = $process.StandardError.ReadToEndAsync()
                })
            }
        }
    }
    foreach ($case in $cases) {
        if (-not $case.Process.WaitForExit(30000)) {
            $case.Process.Kill() # This exact child was launched and is owned by this probe.
            $case.Process.WaitForExit()
            $failures++
            Write-Output ('TIMEOUT: ' + $case.Label)
        }
        $exitCode = $case.Process.ExitCode
        [IO.File]::WriteAllText((Join-Path $case.Working 'stdout.txt'), $case.Output.Result)
        [IO.File]::WriteAllText((Join-Path $case.Working 'stderr.txt'), $case.Error.Result)
        Write-Output ($case.Label + ' exit=' + $exitCode)
        if ($exitCode -ne 0) { $failures++ }
    }
    $leftovers = @(Get-ChildItem -LiteralPath $sharedTemp -Force)
    Write-Output ('processes=' + $cases.Count + ' failures=' + $failures + ' shared_temp_entries=' + $leftovers.Count)
    if ($failures -ne 0 -or $leftovers.Count -ne 0) { throw 'Fixture isolation probe failed; exact per-process outputs retained under run root' }
    Write-Output 'PASS: all independent test processes succeeded with one shared temporary root; no fixture entries remain.'
} finally {
    foreach ($case in $cases) {
        try { if (-not $case.Process.HasExited) { $case.Process.Kill(); $case.Process.WaitForExit() } }
        finally { $case.Process.Dispose() }
    }
}
