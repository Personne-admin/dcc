param(
    [string]$Learndc = "learndc.exe",
    [string]$ToolDir = "",
    [string]$Root = $env:TEMP
)

$ErrorActionPreference = "Stop"
Write-Host "NOTE: this script has never been run. It is the first native Windows check of learndc and is expected to need fixes."

function Fail([string]$Message) {
    Write-Host "FAIL: $Message"
    exit 1
}

function Invoke-Learndc([string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:LearndcPath
    foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(300000)) {
        $p.Kill($true)
        Fail "timeout running learndc $($Arguments -join ' ')"
    }
    [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result; Err = $err.Result }
}

function Expect-Exit([string]$What, [string[]]$Arguments, [int]$Code) {
    $r = Invoke-Learndc $Arguments
    if ($r.Code -ne $Code) {
        Write-Host $r.Out
        Write-Host $r.Err
        Fail "$What: expected exit $Code, got $($r.Code)"
    }
    Write-Host "ok: $What (exit $Code)"
    $r
}

$cmd = Get-Command $Learndc -ErrorAction SilentlyContinue
if (-not $cmd) { Fail "learndc executable not found: $Learndc" }
$script:LearndcPath = $cmd.Source

if ($ToolDir -ne "") {
    $env:PATH = "$ToolDir;$env:PATH"
}
foreach ($tool in @("dcc.exe", "dpm.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool not found on PATH; install the toolchain or pass -ToolDir"
    }
}

$uni = [string]::Concat([char]0x00E9, [char]0x00E8, [char]0x4E2D, [char]0x6587)
$home1 = Join-Path $Root "learndc smoke $uni one"
$home2 = Join-Path $Root "learndc smoke $uni two"
foreach ($h in @($home1, $home2)) {
    if (Test-Path -LiteralPath $h) { Remove-Item -LiteralPath $h -Recurse -Force }
}
New-Item -ItemType Directory -Path $home1 | Out-Null
New-Item -ItemType Directory -Path $home2 | Out-Null
$env:LEARNDC_HOME = $home1
$env:NO_COLOR = "1"

$r = Expect-Exit "--version" @("--version") 0
if ($r.Out -notmatch "^learndc ") { Fail "--version output unexpected: $($r.Out)" }

$r = Expect-Exit "doctor --json" @("doctor", "--json") 0
$doctor = $r.Out | ConvertFrom-Json
if (-not $doctor.ok) { Fail "doctor reports problems: $($r.Out)" }
if ($doctor.items.Count -lt 5) { Fail "doctor listed too few items" }

$r = Expect-Exit "list --json" @("list", "--json") 0
$list = $r.Out | ConvertFrom-Json
$ids = @($list.exercises | ForEach-Object { $_.id })
if ($ids -notcontains "fundamentals/hello-world") { Fail "list does not contain fundamentals/hello-world" }

$r = Expect-Exit "start" @("start", "fundamentals/hello-world") 0
$workspace = Join-Path $home1 "workspaces\fundamentals\hello-world"
$entry = Join-Path $workspace "src\main.dc"
if (-not (Test-Path -LiteralPath $entry)) { Fail "workspace entry file missing: $entry" }

$r = Expect-Exit "check (starter fails)" @("check", "--json") 1
$report = $r.Out | ConvertFrom-Json
if ($report.outcome -ne "failed") { Fail "starter check outcome was $($report.outcome), expected failed" }

$solution = @'
module main;

public Status(Error) main() {
    []const char name = "Ada";
    println("Hello, {}!", name)?;
    println("Welcome to DC.")?;
    return Status::Ok;
}
'@
[System.IO.File]::WriteAllText($entry, $solution.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))

$r = Expect-Exit "check (solution passes)" @("check", "--json") 0
$report = $r.Out | ConvertFrom-Json
if ($report.outcome -ne "passed") { Fail "solution outcome was $($report.outcome)" }
if (-not $report.completed_now) { Fail "solution did not complete the exercise" }

$r = Expect-Exit "progress --json" @("progress", "--json") 0
$progress = $r.Out | ConvertFrom-Json
if ($progress.completed -ne 1) { Fail "progress.completed is $($progress.completed), expected 1" }
$entryProgress = $progress.exercises | Where-Object { $_.id -eq "fundamentals/hello-world" }
if ($entryProgress.status -ne "completed") { Fail "hello-world status is $($entryProgress.status)" }

$exportFile = Join-Path $Root "learndc smoke $uni export.json"
if (Test-Path -LiteralPath $exportFile) { Remove-Item -LiteralPath $exportFile -Force }
[void](Expect-Exit "export" @("export", "--output", $exportFile) 0)
if (-not (Test-Path -LiteralPath $exportFile)) { Fail "export file was not written" }

$env:LEARNDC_HOME = $home2
$r = Expect-Exit "import preview" @("import", "--input", $exportFile) 0
if ($r.Out -notmatch "Preview only") { Fail "import preview did not say it is a preview: $($r.Out)" }
[void](Expect-Exit "import --apply" @("import", "--input", $exportFile, "--apply") 0)
$r = Expect-Exit "progress in the second home" @("progress", "--json") 0
$progress2 = $r.Out | ConvertFrom-Json
if ($progress2.completed -ne 1) { Fail "imported progress.completed is $($progress2.completed), expected 1" }

$env:LEARNDC_HOME = $home1
[void](Expect-Exit "start (variables)" @("start", "fundamentals/variables") 0)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $script:LearndcPath
[void]$psi.ArgumentList.Add("watch")
[void]$psi.ArgumentList.Add("fundamentals/variables")
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$watch = [System.Diagnostics.Process]::Start($psi)
$watchOut = $watch.StandardOutput.ReadToEndAsync()
$null = $watch.StandardError.ReadToEndAsync()
Start-Sleep -Seconds 20
if ($watch.HasExited) { Fail "watch exited by itself with code $($watch.ExitCode)" }
$watch.Kill($true)
if (-not $watch.WaitForExit(30000)) { Fail "watch did not stop after Kill" }
Start-Sleep -Seconds 2
$stray = Get-Process -Name "dpm", "dcc" -ErrorAction SilentlyContinue |
    Where-Object { $_.StartTime -gt $watch.StartTime }
if ($stray) { Fail "processes left behind after stopping watch: $(($stray | ForEach-Object { $_.Name }) -join ', ')" }
Write-Host "ok: watch started and stopped, no stray dpm or dcc processes"

foreach ($h in @($home1, $home2)) { Remove-Item -LiteralPath $h -Recurse -Force }
Remove-Item -LiteralPath $exportFile -Force
Write-Host "PASS: learndc windows smoke"
exit 0
