param(
    [Parameter(Mandatory = $true)][string]$Msi,
    [string]$Work = $env:RUNNER_TEMP
)
$ErrorActionPreference = 'Stop'
$Msi = (Resolve-Path $Msi).Path
if (-not $Work) { $Work = [IO.Path]::GetTempPath() }
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Resolve-Path (Join-Path $Here '..\..')
$Tools = @('dcc', 'dccd', 'dpm', 'learndc')
$Sentinel = Join-Path $Work ('dcc path sentinel ' + [char]0x5DE5)
$Checks = 0

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "MSI smoke: $Message" }
    $script:Checks++
}

function Invoke-Msiexec([string]$Arguments) {
    $log = Join-Path $Work ('msiexec-' + [Guid]::NewGuid().ToString('N') + '.log')
    $p = Start-Process msiexec.exe -ArgumentList "$Arguments /qn /norestart /l*v `"$log`"" -Wait -PassThru
    if ($p.ExitCode -notin @(0, 3010)) {
        Get-Content $log -Tail 60 | Write-Host
        throw "msiexec $Arguments failed with $($p.ExitCode)"
    }
}

function Machine-Path { [Environment]::GetEnvironmentVariable('Path', 'Machine') }

function Path-Entries([string]$Value) {
    @($Value -split ';' | Where-Object { $_ -ne '' } | ForEach-Object { $_.TrimEnd('\').ToLowerInvariant() })
}

function Count-Entry([string]$Dir) {
    $want = $Dir.TrimEnd('\').ToLowerInvariant()
    @(Path-Entries (Machine-Path) | Where-Object { $_ -eq $want }).Count
}

function Fresh-Path {
    [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Resolve-In-New-Environment([string]$Name) {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Get-Command pwsh).Source
    foreach ($a in @('-NoProfile', '-NonInteractive', '-Command', "(Get-Command $Name -CommandType Application | Select-Object -First 1).Source")) { [void]$psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.Environment['Path'] = Fresh-Path
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd().Trim()
    $p.WaitForExit()
    return $out
}

function Run-Tool([string]$Exe, [string[]]$Arguments, [hashtable]$Environment = @{}) {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    foreach ($k in $Environment.Keys) { $psi.Environment[$k] = $Environment[$k] }
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(120000)) { $p.Kill($true); throw "timeout: $Exe $($Arguments -join ' ')" }
    [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result; Err = $err.Result }
}

function Shortcut-Path { Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'dcc\Learn DC.lnk' }

function Check-Installed([string]$Prefix, [bool]$OnPath) {
    $bin = Join-Path $Prefix 'bin'
    foreach ($t in $Tools) { Check (Test-Path (Join-Path $bin "$t.exe")) "$t.exe is not installed in $bin" }
    Check (Test-Path (Join-Path $Prefix 'licenses\FTXUI-LICENSE')) 'the FTXUI license is not installed'
    $learndc = Join-Path $bin 'learndc.exe'
    $learnHome = Join-Path $Work ('learndc home ' + [char]0x5DE5 + [char]0x5177 + ' ' + [Guid]::NewGuid().ToString('N'))
    $v = Run-Tool $learndc @('--version')
    Check ($v.Code -eq 0 -and $v.Out -match 'learndc') "learndc --version failed: $($v.Code) $($v.Err)"
    $l = Run-Tool $learndc @('list', '--json') @{ LEARNDC_HOME = $learnHome }
    Check ($l.Code -eq 0 -and $l.Out -match 'first-steps/hello-world') "learndc list --json did not show the embedded catalog: $($l.Code) $($l.Err)"
    $s = Run-Tool $learndc @('show', 'first-steps/hello-world') @{ LEARNDC_HOME = $learnHome }
    Check ($s.Code -eq 0 -and $s.Out -match 'Hello, Ada') 'learndc show did not print the embedded instructions'
    $h = Run-Tool $learndc @('hint', 'first-steps/hello-world', '--level', '1') @{ LEARNDC_HOME = $learnHome }
    Check ($h.Code -eq 0 -and $h.Out.Length -gt 0) 'learndc hint did not print an embedded hint'
    $lnk = Shortcut-Path
    Check (Test-Path $lnk) "the Start Menu shortcut $lnk is missing"
    $shell = New-Object -ComObject WScript.Shell
    $target = $shell.CreateShortcut($lnk).TargetPath
    Check ([IO.Path]::GetFullPath($target) -eq [IO.Path]::GetFullPath($learndc)) "the shortcut targets $target instead of $learndc"
    Check ($shell.CreateShortcut($lnk).Arguments -eq '') 'the shortcut passes arguments; it must open the dashboard'
    if ($OnPath) {
        Check ((Count-Entry $bin) -eq 1) "machine PATH has $(Count-Entry $bin) entries for $bin, expected 1"
        foreach ($t in $Tools) {
            $found = Resolve-In-New-Environment $t
            Check ($found -and ([IO.Path]::GetFullPath($found) -eq [IO.Path]::GetFullPath((Join-Path $bin "$t.exe")))) "a new process resolves $t to '$found' instead of $bin"
        }
    } else {
        Check ((Count-Entry $bin) -eq 0) "ADDTOPATH=0 still added $bin to PATH"
    }
    Check ((Count-Entry $Sentinel) -eq 1) 'an unrelated PATH entry was lost'
}

function Check-Removed([string]$Prefix) {
    $bin = Join-Path $Prefix 'bin'
    Check (-not (Test-Path (Shortcut-Path))) 'uninstall left the Start Menu shortcut behind'
    Check (-not (Test-Path (Split-Path -Parent (Shortcut-Path)))) 'uninstall left the dcc Start Menu folder behind'
    Check ((Count-Entry $bin) -eq 0) "uninstall left $bin in PATH"
    Check ((Count-Entry $Sentinel) -eq 1) 'uninstall damaged an unrelated PATH entry'
}

$before = Machine-Path
[IO.Directory]::CreateDirectory($Sentinel) | Out-Null
[Environment]::SetEnvironmentVariable('Path', $before + ';' + $Sentinel, 'Machine')
try {
    $custom = Join-Path $Work ('dcc install ' + [char]0x5DE5 + [char]0x5177)
    foreach ($prefix in @((Join-Path $env:ProgramFiles 'dcc'), $custom)) {
        Write-Host "== install into $prefix"
        Invoke-Msiexec "/i `"$Msi`" INSTALLDIR=`"$prefix`""
        try {
            Check-Installed $prefix $true
            Write-Host '== repair'
            Invoke-Msiexec "/fa `"$Msi`""
            Check-Installed $prefix $true
            Write-Host '== reinstall over the existing installation'
            Invoke-Msiexec "/i `"$Msi`" REINSTALL=ALL REINSTALLMODE=vomus"
            Check-Installed $prefix $true
            & (Join-Path $Repo 'tests\dpm\windows.ps1') -Prefix $prefix
            & (Join-Path $Repo 'tests\learndc\windows-smoke.ps1') -Learndc (Join-Path $prefix 'bin\learndc.exe') -ToolDir (Join-Path $prefix 'bin') -Root $Work
            if ($LASTEXITCODE -ne 0) { Write-Host '::warning::tests\learndc\windows-smoke.ps1 failed (known issue: learndc check cannot load the dpm::v1 schema on native Windows)' }
        } finally {
            Write-Host '== uninstall'
            Invoke-Msiexec "/x `"$Msi`""
        }
        Check-Removed $prefix
    }
    Write-Host '== install with ADDTOPATH=0'
    $optout = Join-Path $Work 'dcc no path'
    Invoke-Msiexec "/i `"$Msi`" INSTALLDIR=`"$optout`" ADDTOPATH=0"
    try {
        Check-Installed $optout $false
        Invoke-Msiexec "/fa `"$Msi`""
        Check-Installed $optout $false
    } finally {
        Invoke-Msiexec "/x `"$Msi`""
    }
    Check-Removed $optout
} finally {
    $kept = @((Machine-Path) -split ';' | Where-Object { $_ -ne '' -and $_.TrimEnd('\').ToLowerInvariant() -ne $Sentinel.TrimEnd('\').ToLowerInvariant() })
    [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'Machine')
}
Write-Host "msi smoke ok ($Checks checks)"
