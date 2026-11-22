param([Parameter(Mandatory=$true)][string]$Prefix)
$ErrorActionPreference = 'Stop'
$Prefix = [IO.Path]::GetFullPath($Prefix)
$Dpm = Join-Path $Prefix 'bin/dpm.exe'
foreach ($name in @('dcc.exe', 'dccd.exe', 'dpm.exe', 'ld.lld.exe')) {
    if (!(Test-Path (Join-Path $Prefix ('bin/' + $name)))) { throw "Missing installed binary: $name" }
}
$Work = Join-Path ([IO.Path]::GetTempPath()) ('dcc DPM ' + [char]0x5DE5 + [char]0x5177 + ' ' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($Work) | Out-Null
$Saved = @{}
foreach ($key in @('PATH', 'DCC', 'DPM_HOME', 'DPM_CONFIG', 'DPM_SCHEMA_ROOT', 'COMMAND_TEST', 'CACHE_TEST_EVENT')) {
    $Saved[$key] = [Environment]::GetEnvironmentVariable($key)
}
$env:PATH = "$Prefix/bin;$env:SystemRoot/System32;$env:SystemRoot/System32/WindowsPowerShell/v1.0"
Remove-Item Env:DCC -ErrorAction SilentlyContinue
Remove-Item Env:DPM_SCHEMA_ROOT -ErrorAction SilentlyContinue
$env:DPM_HOME = Join-Path $Work 'cache'
$env:DPM_CONFIG = Join-Path $Work 'no-config.dcon'
$Utf8 = [Text.UTF8Encoding]::new($false)
$Checks = 0
function Check($Condition, $Message) {
    if (!$Condition) { throw $Message }
    $script:Checks++
}
function Write-Utf8($Path, $Text) { [IO.File]::WriteAllText($Path, $Text, $Utf8) }
function Quote-Arg([string]$Value) {
    $result = '"'
    $slashes = 0
    foreach ($c in $Value.ToCharArray()) {
        if ($c -eq '\') { $slashes++; continue }
        if ($c -eq '"') { $result += ('\' * (2 * $slashes + 1)) + '"' }
        else { $result += ('\' * $slashes) + $c }
        $slashes = 0
    }
    return $result + ('\' * (2 * $slashes)) + '"'
}
function Start-Dpm([string]$Directory, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Dpm
    $info.WorkingDirectory = $Directory
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $Utf8
    $info.StandardErrorEncoding = $Utf8
    $info.Arguments = (($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $process = [Diagnostics.Process]::Start($info)
    return @{ Process = $process; Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync() }
}
function Finish-Dpm($Job, [int]$Expected = 0) {
    if (!$Job.Process.WaitForExit(120000)) { $Job.Process.Kill(); throw 'DPM timeout' }
    $text = $Job.Out.GetAwaiter().GetResult() + $Job.Err.GetAwaiter().GetResult()
    $code = $Job.Process.ExitCode
    $Job.Process.Dispose()
    Check ($code -eq $Expected) "DPM exit $code, expected $Expected`n$text"
    return $text
}
function Invoke-Dpm([string]$Directory, [string[]]$Arguments, [int]$Expected = 0) {
    return Finish-Dpm (Start-Dpm $Directory $Arguments) $Expected
}
$event = $null
$holder = $null
$waiter = $null
try {
    Check ((Invoke-Dpm $Work @('--version')) -match '0\.[0-9]+') 'version'
    $Dcc = Join-Path $Prefix 'bin/dcc.exe'
    $Direct = Join-Path $Work ('direct ' + [char]0x5DE5 + '.dc')
    Write-Utf8 $Direct 'module main; public Status(Error) main() { println("Direct PE")?; return Status::Ok; }'
    foreach ($backend in @('llvm', 'custom')) {
        foreach ($level in @('-O0', '-O2')) {
            foreach ($mode in @('-fwhole-program', '-fno-whole-program')) {
                $Exe = Join-Path $Work ("direct $backend$level$mode.exe")
                & $Dcc $Direct -target x86_64-coff -flibdcext windows -fbackend $backend $level $mode -o $Exe
                Check ($LASTEXITCODE -eq 0) "dcc direct PE link failed: $backend $level $mode"
                $text = (& $Exe) -join "`n"
                Check (($LASTEXITCODE -eq 0) -and $text.Contains('Direct PE')) "direct PE run failed: $backend $level $mode"
            }
        }
    }
    Invoke-Dpm $Work @('new', 'hello') | Out-Null
    $Project = Join-Path $Work 'hello'
    $original = [IO.File]::ReadAllText((Join-Path $Project 'src/main.dc'))
    Check ($original.Contains('Status(Error) main')) 'beginner scaffold changed'
    Invoke-Dpm $Project @('build', '--offline') | Out-Null
    Check (Test-Path (Join-Path $Project '.dpm/out/debug/host/hello.exe')) 'missing .exe output'
    Check ((Invoke-Dpm $Project @('run', '--offline')).Contains('Hello, world!')) 'stdout hello'
    Invoke-Dpm $Project @('build', '--profile', 'release', '--offline') | Out-Null
    Check (Test-Path (Join-Path $Project '.dpm/out/release/host/hello.exe')) 'release .exe'
    [IO.Directory]::CreateDirectory((Join-Path $Project 'tests')) | Out-Null
    Write-Utf8 (Join-Path $Project 'tests/basic.dc') 'module basic; public i32 main() { return 0; }'
    Invoke-Dpm $Project @('test', '--offline') | Out-Null
    Write-Utf8 (Join-Path $Project 'src/main.dc') 'module main; public i32 main() { return missing; }'
    Check ((Invoke-Dpm $Project @('build', '--offline') 1).Contains('missing')) 'compiler diagnostics'
    Write-Utf8 (Join-Path $Project 'src/main.dc') $original
    Invoke-Dpm $Work @('new', 'dependency', '--lib') | Out-Null
    $Dependency = Join-Path $Work 'dependency'
    [IO.Directory]::CreateDirectory((Join-Path $Dependency 'src')) | Out-Null
    Write-Utf8 (Join-Path $Dependency 'src/dep.dc') 'module dep; public i32 answer() { return 42; }'
    [IO.Directory]::CreateDirectory((Join-Path $Project 'sub dir')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Project 'data')) | Out-Null
    Write-Utf8 (Join-Path $Project 'data/value.txt') 'asset'
    Write-Utf8 (Join-Path $Project 'record.ps1') @'
$raw = [Environment]::GetCommandLineArgs()
$i = [Array]::IndexOf($raw, '-File') + 1
$output = $raw[$i + 1]
$values = @($raw | Select-Object -Skip ($i + 2))
@{ values = $values; cwd = [Environment]::CurrentDirectory; env = $env:COMMAND_TEST } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $output -Encoding UTF8
'@
    Write-Utf8 (Join-Path $Project 'hold.ps1') @'
[IO.File]::WriteAllText((Join-Path $env:DPM_ROOT 'holder.pid'), [string]$PID)
$event = [Threading.EventWaitHandle]::OpenExisting($env:CACHE_TEST_EVENT)
$event.Set() | Out-Null
Start-Sleep -Seconds 120
'@
    Write-Utf8 (Join-Path $Project 'fail.ps1') 'exit 17'
    Write-Utf8 (Join-Path $Project 'asset.ps1') 'param([string]$Path); [IO.File]::ReadAllText($Path)'
    Write-Utf8 (Join-Path $Project 'project.dcon') @'
dpm::v1::Project {
    name = "hello", version = "0.1.0",
    dependencies = { { location = "path:../dependency" } },
    assets = { { name = "data", location = "path:data" } },
    products = { { name = "hello", entry = "src/main.dc" } },
    tasks = {
        { name = "record", command = { "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "${root}/record.ps1", "${root}/task-result.json" },
          cwd = "sub dir", env = { { name = "COMMAND_TEST", value = "overridden" } } },
        { name = "asset", command = { "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "${root}/asset.ps1", "${asset:data}/value.txt" } },
        { name = "fail", command = { "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "${root}/fail.ps1" } },
        { name = "missing", command = { "dcc-definitely-missing.exe" } },
        { name = "batch", command = { "cmd.exe", "/d", "/s", "/c", "echo batch-ok" } },
        { name = "hold", command = { "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "${root}/hold.ps1" } },
    },
}
'@
    Write-Utf8 (Join-Path $Project 'src/main.dc') @'
module main;
import dep;
public Status(Error) main() {
    Array(i32) values = {};
    defer values.deinit();
    values.push(dep::answer())?;
    println("Value: {}", values.items()[0])?;
    return Status::Ok;
}
'@
    Invoke-Dpm $Project @('lock', '--offline') | Out-Null
    $lock = [IO.File]::ReadAllText((Join-Path $Project 'project.lock.dcon'))
    Check (!$lock.Contains($Work)) 'absolute machine path in shared lockfile'
    Check ((Invoke-Dpm $Project @('run', '--offline')).Contains('Value: 42')) 'local dependency/Array/printing'
    Invoke-Dpm $Project @('lock', '--check', '--offline') | Out-Null
    $env:COMMAND_TEST = 'parent'
    $expected = @('', 'two words', 'a"b', 'tail\', 'a;&|%', ([string][char]0x5DE5 + [char]0x5177))
    Invoke-Dpm $Project (@('task', 'record', '--offline', '--') + $expected) | Out-Null
    $record = Get-Content -LiteralPath (Join-Path $Project 'task-result.json') -Raw | ConvertFrom-Json
    Check (($record.values | ConvertTo-Json -Compress) -eq ($expected | ConvertTo-Json -Compress)) 'argument boundaries/Unicode/no shell'
    Check ($record.env -eq 'overridden') 'environment override'
    Check ([IO.Path]::GetFullPath($record.cwd) -eq [IO.Path]::GetFullPath((Join-Path $Project 'sub dir'))) 'child cwd'
    Check ((Invoke-Dpm $Project @('task', 'asset', '--offline')).Contains('asset')) 'local asset expansion'
    Check ((Invoke-Dpm $Project @('task', 'batch', '--offline')).Contains('batch-ok')) 'explicit batch interpreter'
    Check ((Invoke-Dpm $Project @('task', 'fail', '--offline') 1).Contains('exit 17')) 'nonzero task status'
    Check ((Invoke-Dpm $Project @('task', 'missing', '--offline') 1).Contains('cannot find')) 'launch failure'
    $name = 'Local\DpmPort-' + [Guid]::NewGuid().ToString('N')
    $event = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $name)
    $env:CACHE_TEST_EVENT = $name
    $holder = Start-Dpm $Project @('task', 'hold', '--offline')
    Check ($event.WaitOne(10000)) 'lock holder/task readiness'
    $child = [Diagnostics.Process]::GetProcessById([int][IO.File]::ReadAllText((Join-Path $Project 'holder.pid')))
    $waiter = Start-Dpm $Project @('cache', 'verify')
    Check (!$waiter.Process.WaitForExit(200)) 'concurrent invocation bypassed the cache lock'
    $holder.Process.Kill()
    $holder.Process.WaitForExit()
    Check ($child.WaitForExit(10000)) 'child survived DPM termination/job close'
    $child.Dispose()
    Finish-Dpm $waiter | Out-Null
    $waiter = $null
    $holder.Process.Dispose()
    $holder = $null
    Invoke-Dpm $Project @('lock', '--check', '--offline') | Out-Null
    Write-Host "PASS $Checks native Windows DPM checks under $Prefix"
}
finally {
    foreach ($job in @($holder, $waiter)) {
        if ($null -ne $job) { if (!$job.Process.HasExited) { $job.Process.Kill() }; $job.Process.Dispose() }
    }
    if ($null -ne $event) { $event.Dispose() }
    foreach ($key in $Saved.Keys) {
        if ($null -eq $Saved[$key]) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key, $Saved[$key]) }
    }
    Remove-Item -LiteralPath $Work -Recurse -ErrorAction SilentlyContinue
}
