#Requires -Version 5.1
<#
.SYNOPSIS
    Builds ZeroIV.asi on Windows with CMake and the MSVC toolchain.

.DESCRIPTION
    Windows counterpart of scripts/build-cross-linux.sh. Locates CMake (PATH or
    the copy bundled with Visual Studio), picks a generator, makes sure the
    MinHook sources are present, configures and builds, and leaves the result in
    the same layout the CMake build and the CI use:

        BUILD\BIN\Release\ZeroIV.asi
        BUILD\BIN\Debug\ZeroIV.asi          (-DebugBuild)

    A default ZeroIV.ini is written next to the binary when one is not already
    there; an existing configuration is never overwritten.

.PARAMETER Configuration
    Release (default), RelWithDebInfo or Debug.

.PARAMETER DebugBuild
    Shorthand for -Configuration Debug.

.PARAMETER Logs
    Compile with _LOG defined (the ReleaseWithLogs configuration in
    CMakeSettings.json) to enable the verbose hot-path logging.

.PARAMETER Clean
    Delete BUILD before configuring.

.PARAMETER Jobs
    Parallel compile jobs (default: logical processor count).

.PARAMETER Generator
    auto (default), ninja, vs2026, vs2022 or vs2019. auto uses the newest
    Visual Studio installation with the C++ tools; ninja imports the MSVC
    environment from vcvars64.bat first.

.PARAMETER Proxy
    HTTP/SOCKS proxy used for the MinHook download, e.g.
    http://127.0.0.1:7897. "none" forces direct access. Defaults to
    ZEROIV_PROXY / https_proxy / all_proxy, then probes the usual local ports.

.PARAMETER MinHook
    Use an existing MinHook checkout instead of the submodule/download. A
    directory junction is created at src\vendor\minhook if needed.

.PARAMETER NoIni
    Do not create a default ZeroIV.ini next to the binary.

.PARAMETER Help
    Show usage.

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Logs -Jobs 8

.EXAMPLE
    .\build.ps1 -Clean -Generator ninja -Proxy http://127.0.0.1:7897
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Configuration = 'Release',

    [switch]$DebugBuild,

    [switch]$Logs,

    [switch]$Clean,

    [int]$Jobs = 0,

    [ValidateSet('auto', 'ninja', 'vs2026', 'vs2022', 'vs2019')]
    [string]$Generator = 'auto',

    [string]$Proxy,

    [string]$MinHook,

    [switch]$NoIni,

    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
# Constants

$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).ProviderPath }
$BuildDir = Join-Path $Root 'BUILD'
$MinHookPath = Join-Path $Root 'src\vendor\minhook'
$MinHookCommit = '4a455528f61b5a375b1f9d44e7d296d47f18bb18'
$MinHookUrl = "https://codeload.github.com/TsudaKageyu/minhook/tar.gz/$MinHookCommit"
$FetchTimeout = 120

# Ports common desktop proxies listen on, probed only when nothing is
# configured but a download is actually needed.
$ProxyCandidates = @(
    'http://127.0.0.1:7897',
    'http://127.0.0.1:7890',
    'http://127.0.0.1:10809',
    'http://127.0.0.1:1080',
    'http://127.0.0.1:8080'
)

$GeneratorByMajor = @{
    18 = 'Visual Studio 18 2026'
    17 = 'Visual Studio 17 2022'
    16 = 'Visual Studio 16 2019'
    15 = 'Visual Studio 15 2017'
}
$MajorByName = @{
    'vs2026' = 18
    'vs2022' = 17
    'vs2019' = 16
}

$script:CMake = $null
$script:GeneratorName = $null
$script:MultiConfig = $false
$script:Ninja = $null
$script:VsInstalls = @()
$script:ProxyCache = $null
$script:ProxyChecked = $false

# ---------------------------------------------------------------------------
# Helpers

function Write-Step {
    param([string]$Message)
    Write-Host '==> ' -ForegroundColor Cyan -NoNewline
    Write-Host $Message
}

function Write-Warn {
    param([string]$Message)
    Write-Host 'warning: ' -ForegroundColor Yellow -NoNewline
    Write-Host $Message
}

function Fail {
    param([string]$Message)
    Write-Host 'error: ' -ForegroundColor Red -NoNewline
    Write-Host $Message
    exit 1
}

function Write-Usage {
    Write-Host @'
Builds ZeroIV.asi on Windows with CMake and MSVC.

Usage: .\build.ps1 [options]

  -Configuration C   Release (default), RelWithDebInfo or Debug
  -DebugBuild        Same as -Configuration Debug
  -Logs              Enable the verbose hot-path logging (_LOG)
  -Clean             Remove BUILD before building
  -Jobs N            Parallel compile jobs (default: logical processors)
  -Generator G       auto (default), ninja, vs2026, vs2022, vs2019
  -Proxy URL         HTTP/SOCKS proxy for the MinHook download; "none" for
                     direct access (default: ZEROIV_PROXY / https_proxy /
                     all_proxy, then probes local proxy ports)
  -MinHook DIR       Use an existing MinHook checkout
  -NoIni             Do not create a default ZeroIV.ini next to the binary
  -Help              Show this help

Output: BUILD\BIN\<Configuration>\ZeroIV.asi (+ ZeroIV.ini)

Examples:
  .\build.ps1
  .\build.ps1 -Logs -Jobs 8
  .\build.ps1 -Clean -Generator ninja

If the script is blocked by the execution policy, run:
  powershell -ExecutionPolicy Bypass -File .\build.ps1
'@
}

# Locates vswhere, the Visual Studio installer query tool.
function Get-VsWhere {
    $installerRoot = ${env:ProgramFiles(x86)}
    if (-not $installerRoot) { $installerRoot = $env:ProgramFiles }
    if ($installerRoot) {
        $vswhere = Join-Path $installerRoot 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) { return $vswhere }
    }
    $cmd = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# Returns every Visual Studio installation that carries the x64 C++ tools,
# newest first. vswhere is authoritative; the directory probe covers installations
# where vswhere itself is missing.
function Get-VsInstallations {
    $result = New-Object System.Collections.Generic.List[object]
    $vswhere = Get-VsWhere
    if ($vswhere) {
        $json = & $vswhere -all -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json 2>$null
        if ($json) {
            try {
                # Windows PowerShell returns native output as an array of lines;
                # parse the whole document at once.
                $jsonText = $json -join "`n"
                # Assign first: piping straight into ConvertFrom-Json inside @()
                # makes Windows PowerShell merge a JSON array into one object
                # whose properties are arrays.
                $parsed = $jsonText | ConvertFrom-Json
                foreach ($item in @($parsed)) {
                    $parts = "$($item.installationVersion)" -split '\.'
                    if ($parts.Count -lt 1) { continue }
                    $major = 0
                    if (-not [int]::TryParse($parts[0], [ref]$major)) { continue }
                    $result.Add([pscustomobject]@{
                        Path    = $item.installationPath
                        Version = $item.installationVersion
                        Major   = $major
                        Name    = $item.displayName
                    })
                }
            } catch {
                Write-Verbose "vswhere output could not be parsed: $_"
            }
        }
    }

    if ($result.Count -eq 0) {
        foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if (-not $base) { continue }
            $pattern = Join-Path $base 'Microsoft Visual Studio\*\*'
            foreach ($dir in (Get-Item $pattern -ErrorAction SilentlyContinue)) {
                $majorText = Split-Path (Split-Path $dir.FullName -Parent) -Leaf
                $major = 0
                if (-not [int]::TryParse($majorText, [ref]$major)) { continue }
                if (-not (Test-Path (Join-Path $dir.FullName 'VC\Auxiliary\Build\vcvars64.bat'))) { continue }
                $result.Add([pscustomobject]@{
                    Path    = $dir.FullName
                    Version = "$major.0"
                    Major   = $major
                    Name    = "$majorText $(Split-Path $dir.FullName -Leaf)"
                })
            }
        }
    }

    return @($result | Sort-Object { $_.Major } -Descending)
}

$script:VsInstalls = @(Get-VsInstallations)

# CMake is either on PATH, taken from ZEROIV_CMAKE, or the copy Visual Studio
# ships under Common7\IDE\CommonExtensions.
function Find-CMake {
    if ($env:ZEROIV_CMAKE -and (Test-Path $env:ZEROIV_CMAKE)) { return $env:ZEROIV_CMAKE }
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($install in $script:VsInstalls) {
        $candidate = Join-Path $install.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Test-GeneratorSupported {
    param([string]$Name)
    $help = & $script:CMake --help 2>&1 | Out-String
    return $help.Contains($Name)
}

function Find-Ninja {
    $cmd = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($install in $script:VsInstalls) {
        $candidate = Join-Path $install.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

# Imports cl.exe/INCLUDE/LIB/PATH from vcvars64.bat into this process. The batch
# file is wrapped in a temporary script so the nested quoting stays readable.
function Import-MsvcEnvironment {
    param([object]$Install)

    $vcvars = Join-Path $Install.Path 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        Fail "vcvars64.bat not found in $($Install.Path).`nInstall the 'Desktop development with C++' workload."
    }

    Write-Step "MSVC environment: $vcvars"
    $wrapper = [IO.Path]::Combine([IO.Path]::GetTempPath(), "zeroiv-vcvars-$PID.bat")
    try {
        $content = "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`nset"
        Set-Content -Path $wrapper -Value $content -Encoding ASCII
        $lines = & cmd.exe /d /c $wrapper
        if ($LASTEXITCODE -ne 0) {
            Fail "vcvars64.bat failed (exit $LASTEXITCODE)."
        }
        foreach ($line in $lines) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
    } finally {
        Remove-Item $wrapper -Force -ErrorAction SilentlyContinue
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Fail 'the MSVC environment was imported but cl.exe is still missing.'
    }
}

function Initialize-Toolchain {
    if (-not (Test-Path (Join-Path $Root 'CMakeLists.txt'))) {
        Fail "CMakeLists.txt not found in $Root; run the script from the repository root."
    }

    $script:CMake = Find-CMake
    if (-not $script:CMake) {
        Fail @'
CMake was not found.
Install CMake (https://cmake.org/download/) and put it on PATH, install the
Visual Studio "Desktop development with C++" workload, or set ZEROIV_CMAKE to a
cmake.exe.
'@
    }
    Write-Step "cmake: $script:CMake ($(& $script:CMake --version | Select-Object -First 1))"

    $chosenInstall = $null
    if ($Generator -eq 'ninja') {
        $script:GeneratorName = 'Ninja'
        $script:MultiConfig = $false
        if ($script:VsInstalls.Count -eq 0) {
            Fail 'Ninja needs the MSVC toolchain, but no Visual Studio C++ installation was found.'
        }
        $chosenInstall = $script:VsInstalls[0]
        $script:Ninja = Find-Ninja
        if (-not $script:Ninja) {
            Fail @'
ninja.exe was not found.
Install Ninja and put it on PATH, or use -Generator vs2022 (the Visual Studio
generator needs no Ninja).
'@
        }
        Write-Step "ninja: $script:Ninja"
    } else {
        $major = $null
        if ($Generator -ne 'auto') { $major = $MajorByName[$Generator] }

        $candidates = @()
        foreach ($install in $script:VsInstalls) {
            if (-not $GeneratorByMajor.ContainsKey($install.Major)) { continue }
            if ($null -ne $major -and $install.Major -ne $major) { continue }
            $candidates += [pscustomobject]@{
                Generator = $GeneratorByMajor[$install.Major]
                Install   = $install
            }
        }

        foreach ($candidate in $candidates) {
            if (Test-GeneratorSupported $candidate.Generator) {
                $script:GeneratorName = $candidate.Generator
                $chosenInstall = $candidate.Install
                break
            }
        }

        if (-not $script:GeneratorName) {
            if ($Generator -eq 'auto' -and (Find-Ninja) -and $script:VsInstalls.Count -gt 0) {
                Write-Warn 'no supported Visual Studio CMake generator found; falling back to Ninja'
                $script:GeneratorName = 'Ninja'
                $script:MultiConfig = $false
                $chosenInstall = $script:VsInstalls[0]
                $script:Ninja = Find-Ninja
                Import-MsvcEnvironment $chosenInstall
            } else {
                $known = ($script:VsInstalls | ForEach-Object { "VS $($_.Major) at $($_.Path)" }) -join "`n  "
                Fail @"
no usable Visual Studio generator for -Generator $Generator.
Detected installations:
  $(if ($known) { $known } else { '(none)' })
Install the 'Desktop development with C++' workload or pick another -Generator.
"@
            }
        } else {
            $script:MultiConfig = $true
        }
    }

    if ($script:GeneratorName -eq 'Ninja') {
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
            Import-MsvcEnvironment $chosenInstall
        }
        $ninjaDir = Split-Path $script:Ninja -Parent
        if (($env:PATH -split ';') -notcontains $ninjaDir) {
            $env:PATH = "$ninjaDir;$env:PATH"
        }
    } else {
        Write-Step "Visual Studio: $($chosenInstall.Name) at $($chosenInstall.Path)"
    }

    Write-Step "generator: $script:GeneratorName (configuration $Configuration)"
}

# Resolves the proxy lazily: nothing is probed until the network is needed.
function Test-ProxyAccess {
    param([string]$Url)
    try {
        $null = Invoke-WebRequest -Uri 'https://github.com' -Proxy $Url -Method Head `
            -TimeoutSec 8 -UseBasicParsing
        return $true
    } catch {
        return $false
    }
}

function Get-NetworkProxy {
    if ($script:ProxyChecked) { return $script:ProxyCache }
    $script:ProxyChecked = $true

    $configured = $Proxy
    if (-not $configured) { $configured = $env:ZEROIV_PROXY }
    if (-not $configured) { $configured = $env:https_proxy }
    if (-not $configured) { $configured = $env:HTTPS_PROXY }
    if (-not $configured) { $configured = $env:all_proxy }
    if (-not $configured) { $configured = $env:ALL_PROXY }

    if ($configured -and $configured -notin @('none', 'off')) {
        $script:ProxyCache = $configured
        return $script:ProxyCache
    }
    if ($configured -in @('none', 'off')) {
        $script:ProxyCache = $null
        return $null
    }

    foreach ($candidate in $ProxyCandidates) {
        if (Test-ProxyAccess $candidate) {
            Write-Warn "no proxy configured, using detected $candidate"
            $script:ProxyCache = $candidate
            return $script:ProxyCache
        }
    }

    $script:ProxyCache = $null
    return $null
}

# git wrapper: bounded remote operations and no interactive credential prompts.
function Invoke-Git {
    param([string[]]$Arguments)

    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { return $false }

    $gitArgs = @('-c', 'http.lowSpeedLimit=1000', '-c', 'http.lowSpeedTime=120')
    $proxyUrl = Get-NetworkProxy
    if ($proxyUrl) {
        $gitArgs += @('-c', "http.proxy=$proxyUrl", '-c', "https.proxy=$proxyUrl")
    }
    $gitArgs += $Arguments

    $previous = $env:GIT_TERMINAL_PROMPT
    $env:GIT_TERMINAL_PROMPT = '0'
    try {
        & $git.Source @gitArgs
        return ($LASTEXITCODE -eq 0)
    } finally {
        $env:GIT_TERMINAL_PROMPT = $previous
    }
}

function Get-RemoteFile {
    param([string]$Url, [string]$Destination)

    $proxyUrl = Get-NetworkProxy
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        $curlArgs = @('--fail', '--silent', '--show-error', '--location',
            '--retry', '2', '--retry-delay', '1',
            '--connect-timeout', '15', '--max-time', "$FetchTimeout")
        if ($proxyUrl) { $curlArgs += @('--proxy', $proxyUrl) }
        $curlArgs += @('-o', $Destination, $Url)
        & $curl.Source @curlArgs
        return ($LASTEXITCODE -eq 0)
    }

    $options = @{
        Uri             = $Url
        OutFile         = $Destination
        TimeoutSec      = $FetchTimeout
        UseBasicParsing = $true
    }
    if ($proxyUrl) { $options.Proxy = $proxyUrl }
    try {
        Invoke-WebRequest @options
        return $true
    } catch {
        Write-Verbose "download failed: $_"
        return $false
    }
}

# Makes src\vendor\minhook usable: an initialized submodule, a -MinHook junction,
# or the pinned upstream tarball when the submodule remote cannot be reached.
function Initialize-MinHook {
    $header = Join-Path $MinHookPath 'include\MinHook.h'
    if (Test-Path $header) {
        Write-Step "MinHook: $MinHookPath"
        return
    }

    if ($MinHook) {
        $source = (Resolve-Path $MinHook).ProviderPath
        if (-not (Test-Path (Join-Path $source 'include\MinHook.h'))) {
            Fail "-MinHook $source does not contain include\MinHook.h"
        }
        if (Test-Path $MinHookPath) {
            $existing = @(Get-ChildItem -Force $MinHookPath -ErrorAction SilentlyContinue)
            if ($existing.Count -gt 0) {
                Fail "$MinHookPath is not empty but has no include\MinHook.h; remove it first."
            }
            Remove-Item $MinHookPath -Force
        }
        New-Item -ItemType Junction -Path $MinHookPath -Target $source | Out-Null
        Write-Step "MinHook: junction to $source"
        return
    }

    if (Test-Path $MinHookPath) {
        $existing = @(Get-ChildItem -Force $MinHookPath -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) {
            Fail @"
$MinHookPath exists but does not contain include\MinHook.h.
Remove it, or point the build at a local checkout with -MinHook <dir>.
"@
        }
        Remove-Item $MinHookPath -Force
    }

    if (Get-Command git.exe -ErrorAction SilentlyContinue) {
        Write-Step 'MinHook: fetching the submodule'
        if (Invoke-Git @('submodule', 'update', '--init', '--depth', '1', 'src/vendor/minhook')) {
            if (Test-Path $header) {
                Write-Step 'MinHook: submodule'
                return
            }
        }
        Write-Warn 'submodule update failed; downloading the pinned upstream sources'
    }

    $stage = Join-Path $BuildDir 'minhook-stage'
    $archive = Join-Path $BuildDir 'minhook.tar.gz'
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tar) {
        Fail @'
MinHook sources are missing and tar.exe is unavailable (Windows 10 1803+ ships it).
Run: git submodule update --init --recursive
or:  .\build.ps1 -MinHook <path-to-minhook>
'@
    }

    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Remove-Item $stage, $archive -Recurse -Force -ErrorAction SilentlyContinue

    Write-Step "MinHook: downloading $MinHookCommit"
    if (-not (Get-RemoteFile $MinHookUrl $archive)) {
        Remove-Item $stage, $archive -Recurse -Force -ErrorAction SilentlyContinue
        Fail @'
could not download MinHook.
Behind a proxy? pass -Proxy http://127.0.0.1:7897 (or set ZEROIV_PROXY).
Already have the sources? pass -MinHook <path-to-minhook>
Or run: git submodule update --init --recursive
'@
    }

    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    & $tar.Source -xzf $archive -C $stage
    if ($LASTEXITCODE -ne 0) { Fail 'could not extract the MinHook archive' }

    $extracted = @(Get-ChildItem -Force $stage -Directory)
    if ($extracted.Count -ne 1) { Fail 'unexpected MinHook archive layout' }

    New-Item -ItemType Directory -Force -Path $MinHookPath | Out-Null
    Get-ChildItem -Force $extracted[0].FullName | Move-Item -Destination $MinHookPath -Force
    Remove-Item $stage, $archive -Recurse -Force -ErrorAction SilentlyContinue

    if (-not (Test-Path $header)) { Fail "MinHook download did not yield include\MinHook.h" }
    Write-Step 'MinHook: pinned source tree'
}

# A cache made with another generator cannot be reused in place.
function Test-CacheGenerator {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return }
    $match = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' -List
    if (-not $match) { return }
    $cached = $match.Matches[0].Groups[1].Value.Trim()
    if ($cached -ne $script:GeneratorName) {
        Write-Warn "generator changed ($cached -> $($script:GeneratorName)); removing BUILD"
        Remove-Item $BuildDir -Recurse -Force
    }
}

function Invoke-Configure {
    Test-CacheGenerator

    $configureArgs = @('-S', $Root, '-B', $BuildDir, '-G', $script:GeneratorName)
    if ($script:MultiConfig) {
        $configureArgs += @('-A', 'x64')
    } else {
        $configureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
    }
    $configureArgs += "-DENABLE_LOGS=$(if ($Logs) { 'ON' } else { 'OFF' })"
    # CMake 4 rejects compatibility with CMake < 3.5, which older MinHook
    # revisions still request. The variable is ignored by CMake 3.x.
    $configureArgs += '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'

    Write-Step 'configuring'
    & $script:CMake @configureArgs
    if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed' }
}

function Invoke-Build {
    $buildArgs = @('--build', $BuildDir)
    if ($script:MultiConfig) { $buildArgs += @('--config', $Configuration) }
    $buildArgs += @('--parallel', "$Jobs")

    Write-Step "building $Configuration ($Jobs job(s))"
    & $script:CMake @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
}

function Write-DefaultIni {
    param([string]$Directory)

    $ini = Join-Path $Directory 'ZeroIV.ini'
    if (Test-Path $ini) { return }

    $content = @'
; ZeroIV-Loader configuration.
; Missing keys are re-added on launch; delete the file to restore defaults.

[config]
; Opens a debug console window next to the game.
console = false
; Mounts the newmods/ override devices over platform:/common:/dlcpacks:.
; Only directories that actually exist are mounted. Set to false to use the
; stock devices and rely on plain mods/ redirection only.
modsOverride = true

[log]
; Per-file-operation traces from the device/file hooks. They fire hundreds of
; times per second while GTA streams assets, so each category is opt-in; the
; loader additionally throttles every category and collapses repeated lines.
; info = every fiDevice / file-read call
; mods = mods/ path redirections and RPF overrides
; rpf  = RPF encryption traces
; device = fiDevice virtual method calls (very chatty)
info = false
mods = false
rpf = false
device = false

[debugConsole]
; Streams the log into ZeroIV's debug console over loopback.
; The port must match the listener in ZeroIV (logbridge.go).
enabled = true
host = 127.0.0.1
port = 27183
'@
    Set-Content -Path $ini -Value $content -Encoding UTF8
    Write-Step "wrote default $ini"
}

# Best effort: list the DLLs the loader links against.
function Show-Imports {
    param([string]$Binary)

    $dumpbin = $null
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $dumpbin = $cmd.Source
    } else {
        foreach ($install in $script:VsInstalls) {
            $found = Get-ChildItem (Join-Path $install.Path 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') `
                -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
            if ($found) {
                $dumpbin = $found.FullName
                break
            }
        }
    }
    if (-not $dumpbin) { return }

    try {
        $output = & $dumpbin /nologo /dependents $Binary 2>$null
        $names = @($output | ForEach-Object {
                if ($_ -match '^\s+([^\s]+\.dll)\s*$') { $matches[1] }
            } | Select-Object -Unique)
        if ($names.Count -gt 0) {
            Write-Step "imports: $($names -join ' ')"
        }
    } catch {
        Write-Verbose "could not inspect imports: $_"
    }
}

# ---------------------------------------------------------------------------
# Main

if ($Help) {
    Write-Usage
    exit 0
}

if ($DebugBuild) { $Configuration = 'Debug' }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Jobs -lt 1) { Fail "invalid job count: $Jobs" }

Write-Step "repository: $Root"
Initialize-Toolchain

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "cleaning $BuildDir"
    Remove-Item $BuildDir -Recurse -Force
}

Initialize-MinHook
Invoke-Configure
Invoke-Build

$outputDir = Join-Path $BuildDir "BIN\$Configuration"
$binary = Join-Path $outputDir 'ZeroIV.asi'
if (-not (Test-Path $binary)) {
    $produced = @(Get-ChildItem $BuildDir -Filter 'ZeroIV.asi' -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
    $hint = if ($produced.Count -gt 0) { "`nFound instead:`n  $($produced -join "`n  ")" } else { '' }
    Fail "build succeeded but $binary is missing.$hint"
}

if (-not $NoIni) {
    Write-DefaultIni $outputDir
}

$sizeMb = [math]::Round((Get-Item $binary).Length / 1MB, 2)
Write-Step "built $binary ($sizeMb MB)"
Show-Imports $binary

Write-Host @'

Next steps:
  - Copy ZeroIV.asi (and ZeroIV.ini, optional) into the GTA V folder.
  - Start ZeroIV and open its Debug Console to watch the
    loader's log live; the same lines go to ZeroIV.log next to the ASI.
'@
