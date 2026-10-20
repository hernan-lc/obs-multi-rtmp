[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Build-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "A 64-bit system is required to build the project."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The obs-studio PowerShell build script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Build {
    trap {
        Pop-Location -Stack BuildTemp -ErrorAction 'SilentlyContinue'
        Write-Error $_
        Log-Group
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpec = Get-Content -LiteralPath (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    if ( [string]::IsNullOrWhiteSpace($ProductName) ) {
        throw 'buildspec.json must define the plugin name.'
    }

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach($Utility in $UtilityFunctions) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    Push-Location -Stack BuildTemp
    Ensure-Location $ProjectRoot

    $CmakeArgs = @('--preset', "windows-ci-${Target}")
    $CmakeBuildArgs = @('--build')
    $CmakeInstallArgs = @()

    if ( $DebugPreference -eq 'Continue' ) {
        $CmakeArgs += ('--debug-output')
        $CmakeBuildArgs += ('--verbose')
        $CmakeInstallArgs += ('--verbose')
    }

    $CmakeBuildArgs += @(
        '--preset', "windows-${Target}"
        '--config', $Configuration
        # Packaging needs the plugin and its dependencies; tests can still
        # be built explicitly through the normal CMake build presets.
        '--target', $ProductName
        '--parallel'
        '--', '/consoleLoggerParameters:Summary', '/noLogo'
    )

    $CmakeInstallArgs += @(
        '--install', "build_${Target}"
        '--prefix', "${ProjectRoot}/release/${Configuration}"
        '--config', $Configuration
    )

    Log-Group "Configuring ${ProductName}..."
    Invoke-External cmake @CmakeArgs

    # The official Windows builds must ship a working login backend: verify
    # the configured one instead of trusting a successful compilation.
    $BackendMarker = Join-Path $ProjectRoot "build_${Target}/tiktok-browser-backend.txt"
    if ( ! ( Test-Path -LiteralPath $BackendMarker ) ) {
        throw "The build did not report a TikTok login browser backend (${BackendMarker} is missing)."
    }
    $Backend = (Get-Content -LiteralPath $BackendMarker -Raw).Trim()
    if ( $Backend -ne 'webview2' ) {
        throw "The Windows build must use the WebView2 login backend, but '${Backend}' was configured."
    }
    Log-Information "TikTok login backend: ${Backend}"

    Log-Group "Building ${ProductName}..."
    Invoke-External cmake @CmakeBuildArgs

    # The WebView2 loader has to sit next to the plugin, otherwise the
    # login window cannot start.
    $PluginDir = Join-Path $ProjectRoot "build_${Target}/${Configuration}"
    $Loader = Get-ChildItem -Path $PluginDir -Filter 'WebView2Loader.dll' -Recurse -ErrorAction 'SilentlyContinue' | Select-Object -First 1
    if ( ! $Loader ) {
        throw "WebView2Loader.dll was not produced next to the plugin."
    }
    Log-Information "WebView2 loader: $($Loader.FullName)"

    Log-Group "Installing ${ProductName}..."
    Invoke-External cmake @CmakeInstallArgs

    $InstalledLoader = Join-Path $ProjectRoot "release/${Configuration}/${ProductName}/bin/64bit/WebView2Loader.dll"
    if ( ! ( Test-Path -LiteralPath $InstalledLoader ) ) {
        throw "WebView2Loader.dll is missing from the installed plugin layout (${InstalledLoader})."
    }

    Pop-Location -Stack BuildTemp
    Log-Group
}

Build
