[CmdletBinding(PositionalBinding=$false)]
param(
    [string]$BuildDir = '',
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')][string]$BuildType = 'Release',
    [ValidateSet('ON','OFF')][string]$Wx = 'ON',
    [ValidateSet('ON','OFF')][string]$RequireWx = 'OFF',
    [ValidateSet('ON','OFF')][string]$Cli = 'ON',
    [ValidateSet('ON','OFF')][string]$MinimalRelease = 'ON',
    [int]$Parallel = 0,
    [string]$Target = '',
    [string]$Generator = '',
    [string]$Platform = '',
    [string]$NeoSharedRoot = '',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$VcpkgTriplet = 'x64-windows-static',
    [switch]$NoVcpkg,
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ExtraCMakeArgs
)

$ErrorActionPreference = 'Stop'

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectName = 'NeoBIF'
$ProjectPrefix = 'NEOBIF'
$CMakeLists = Join-Path $RootDir 'CMakeLists.txt'

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RootDir 'build'
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RootDir $BuildDir
}
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    $RootPath = [System.IO.Path]::GetFullPath($RootDir).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $BuildPath = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $Prefix = $RootPath + [System.IO.Path]::DirectorySeparatorChar
    if (-not $BuildPath.StartsWith($Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $BuildPath"
    }
    Remove-Item -Recurse -Force -LiteralPath $BuildPath
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeArgs = @()
if (-not [string]::IsNullOrWhiteSpace($Generator)) {
    $cmakeArgs += @('-G', $Generator)
    if (-not [string]::IsNullOrWhiteSpace($Platform)) {
        $cmakeArgs += @('-A', $Platform)
    }
}
$cmakeArgs += @(
    '-S', $RootDir,
    '-B', $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DNEO_MINIMAL_RELEASE=$MinimalRelease"
)

$wxOption = "${ProjectPrefix}_BUILD_WX_GUI"
$requireWxOption = "${ProjectPrefix}_REQUIRE_WX_GUI"
$cliOption = "${ProjectPrefix}_BUILD_CLI"
if (Select-String -Path $CMakeLists -Pattern ([regex]::Escape($wxOption)) -Quiet) {
    $cmakeArgs += "-D$wxOption=$Wx"
}
if (Select-String -Path $CMakeLists -Pattern ([regex]::Escape($requireWxOption)) -Quiet) {
    $cmakeArgs += "-D$requireWxOption=$RequireWx"
}
if (Select-String -Path $CMakeLists -Pattern ([regex]::Escape($cliOption)) -Quiet) {
    $cmakeArgs += "-D$cliOption=$Cli"
}

if ([string]::IsNullOrWhiteSpace($NeoSharedRoot)) {
    $resolvedShared = & cmake -P (Join-Path $RootDir 'cmake/NeoSharedSource.cmake')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $NeoSharedRoot = ($resolvedShared | Select-Object -Last 1).Trim()
}

$VcpkgOverlayPorts = $null
if (-not [string]::IsNullOrWhiteSpace($NeoSharedRoot)) {
    if (-not [System.IO.Path]::IsPathRooted($NeoSharedRoot)) {
        $NeoSharedRoot = Join-Path $RootDir $NeoSharedRoot
    }
    $NeoSharedRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($NeoSharedRoot)
    if (-not (Test-Path -LiteralPath (Join-Path $NeoSharedRoot 'CMakeLists.txt'))) {
        throw "neoshared CMakeLists.txt not found under: $NeoSharedRoot"
    }
    $cmakeArgs += "-DNEOSHARED_ROOT=$NeoSharedRoot"

    $OverlayCandidate = Join-Path $NeoSharedRoot 'vcpkg-ports'
    if (Test-Path -LiteralPath (Join-Path $OverlayCandidate 'wxwidgets\vcpkg.json')) {
        $VcpkgOverlayPorts = $OverlayCandidate
    }
}

if (-not $NoVcpkg) {
    if ([string]::IsNullOrWhiteSpace($VcpkgRoot) -and
        -not [string]::IsNullOrWhiteSpace($env:VCPKG_INSTALLATION_ROOT)) {
        $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
    }

    if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        $SiblingVcpkg = Join-Path (Split-Path $RootDir -Parent) 'vcpkg'
        if (Test-Path -LiteralPath (Join-Path $SiblingVcpkg 'scripts\buildsystems\vcpkg.cmake')) {
            $VcpkgRoot = $SiblingVcpkg
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        if (-not [System.IO.Path]::IsPathRooted($VcpkgRoot)) {
            $VcpkgRoot = Join-Path $RootDir $VcpkgRoot
        }
        $VcpkgRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VcpkgRoot)
        $ToolchainFile = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
        if (-not (Test-Path -LiteralPath $ToolchainFile)) {
            throw "vcpkg toolchain file not found: $ToolchainFile"
        }

        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
        if (-not [string]::IsNullOrWhiteSpace($VcpkgTriplet)) {
            $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
        }
        if (-not [string]::IsNullOrWhiteSpace($VcpkgOverlayPorts)) {
            $cmakeArgs += "-DVCPKG_OVERLAY_PORTS=$VcpkgOverlayPorts"
        }
    }
}

if ($ExtraCMakeArgs) {
    $cmakeArgs += $ExtraCMakeArgs
}

Write-Host "Configuring $ProjectName in $BuildDir"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @('--build', $BuildDir, '--config', $BuildType)
if ($Parallel -gt 0) {
    $buildArgs += @('--parallel', [string]$Parallel)
}
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $buildArgs += @('--target', $Target)
}

Write-Host "Building $ProjectName"
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
