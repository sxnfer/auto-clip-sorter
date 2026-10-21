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
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*-installer.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    # The installer goes to release/, not release/<cfg>/, so it can never end up inside the zip.
    Log-Group "Building installer..."

    $IsccCommand = Get-Command -Name 'ISCC.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ( $IsccCommand ) {
        $Iscc = $IsccCommand.Source
    } else {
        $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    }
    if ( ! ( Test-Path -LiteralPath $Iscc -PathType Leaf ) ) {
        throw "Inno Setup (ISCC.exe) was not found on PATH or at '${Iscc}'. Install Inno Setup 6."
    }
    Write-Debug "Using Inno Setup compiler at ${Iscc}"

    $SourceDir = [System.IO.Path]::GetFullPath("${ProjectRoot}/release/${Configuration}/${ProductName}")
    $OutputDir = [System.IO.Path]::GetFullPath("${ProjectRoot}/release")
    $IssFile = [System.IO.Path]::GetFullPath("${ProjectRoot}/installer/clip-atlas.iss")
    $InstallerName = "${OutputName}-installer"
    $InstallerPath = Join-Path -Path $OutputDir -ChildPath "${InstallerName}.exe"

    if ( ! ( Test-Path -LiteralPath $SourceDir -PathType Container ) ) {
        throw "Staged plugin folder not found, so no installer was built: ${SourceDir}"
    }
    $PluginDll = "${SourceDir}/bin/64bit/${ProductName}.dll"
    if ( ! ( Test-Path -LiteralPath $PluginDll -PathType Leaf ) ) {
        throw "Plugin DLL not found, so no installer was built: ${PluginDll}"
    }

    $IsccArgs = @(
        "/DMyAppVersion=${ProductVersion}"
        "/DMySourceDir=${SourceDir}"
        "/DMyOutputDir=${OutputDir}"
        "/DMyOutputName=${InstallerName}"
        $IssFile
    )

    Invoke-External $Iscc @IsccArgs

    if ( ! ( Test-Path -LiteralPath $InstallerPath -PathType Leaf ) ) {
        throw "Inno Setup finished without an error, but no installer was created: ${InstallerPath}"
    }
    Log-Group
}

Package
