param(
    [string]$BuildDir = "",
    [string]$IdfExport = "",
    [switch]$ResetManagedComponents,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildArgs
)

$ErrorActionPreference = "Stop"

function Resolve-IdfExportScript {
    param([string]$PreferredPath)

    if (-not [string]::IsNullOrWhiteSpace($PreferredPath)) {
        if (Test-Path -LiteralPath $PreferredPath) {
            return (Resolve-Path -LiteralPath $PreferredPath).Path
        }
        throw "ESP-IDF export script not found: $PreferredPath"
    }

    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
        $candidates += (Join-Path $env:IDF_PATH "export.ps1")
    }

    $candidates += @(
        "C:\esp\v6.0\esp-idf\export.ps1",
        "C:\esp\esp-idf\export.ps1",
        (Join-Path $env:USERPROFILE ".platformio\packages\framework-espidf\export.ps1")
    )

    foreach ($path in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    throw "ESP-IDF export.ps1 not found. Use -IdfExport <path>."
}

function Ensure-CodecDevCompatibilityPatch {
    param([string]$ProjectRootPath)

    $codecCmake = Join-Path $ProjectRootPath "managed_components\espressif__esp_codec_dev\CMakeLists.txt"
    if (Test-Path -LiteralPath $codecCmake) {
        $cmakeText = Get-Content -LiteralPath $codecCmake -Raw
        $updatedCmake = $cmakeText

        if ($updatedCmake -notmatch "REQUIRES\s+driver\s+esp_driver_gpio\s+esp_driver_i2c\s+esp_driver_i2s\s+esp_driver_spi") {
            $updatedCmake = [regex]::Replace(
                $updatedCmake,
                "REQUIRES\s+driver\b",
                "REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi",
                1
            )
        }

        if ($updatedCmake -ne $cmakeText) {
            Write-Host "[start-clean-build] Patching esp_codec_dev CMake requirements"
            Set-Content -LiteralPath $codecCmake -Value $updatedCmake -NoNewline
        }
    }

    $codecSpi = Join-Path $ProjectRootPath "managed_components\espressif__esp_codec_dev\platform\audio_codec_ctrl_spi.c"
    if (Test-Path -LiteralPath $codecSpi) {
        $spiText = Get-Content -LiteralPath $codecSpi -Raw
        $updatedSpi = $spiText

        if ($updatedSpi -notmatch '#include "esp_idf_version.h"') {
            $updatedSpi = $updatedSpi -replace '#include "esp_err.h"\r?\n', "#include `"esp_err.h`"`r`n#include `"esp_idf_version.h`"`r`n"
        }

        if ($updatedSpi -ne $spiText) {
            Write-Host "[start-clean-build] Patching esp_codec_dev SPI source for ESP-IDF version macros"
            Set-Content -LiteralPath $codecSpi -Value $updatedSpi -NoNewline
        }
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $hostName = $env:COMPUTERNAME
    if ([string]::IsNullOrWhiteSpace($hostName)) {
        $hostName = "default"
    }
    $BuildDir = "build_$hostName"
}

if (-not $BuildArgs -or $BuildArgs.Count -eq 0) {
    $BuildArgs = @("build")
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRootObj = Resolve-Path (Join-Path $scriptDir "..")
$projectRoot = $projectRootObj.Path
$idfExportScript = Resolve-IdfExportScript -PreferredPath $IdfExport

Write-Host "[start-clean-build] Project root: $projectRoot"
Write-Host "[start-clean-build] Build directory: $BuildDir"
Write-Host "[start-clean-build] ESP-IDF export: $idfExportScript"
Write-Host "[start-clean-build] Reset managed_components: $ResetManagedComponents"

Push-Location $projectRoot
try {
    . $idfExportScript

    if ($ResetManagedComponents) {
        $managedPath = Join-Path $projectRoot "managed_components"
        if (Test-Path -LiteralPath $managedPath) {
            $managedResolved = (Resolve-Path -LiteralPath $managedPath).Path
            if (-not $managedResolved.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to delete path outside project root: $managedResolved"
            }
            Write-Host "[start-clean-build] Removing $managedResolved"
            Remove-Item -LiteralPath $managedResolved -Recurse -Force
        }
        if (-not (Test-Path -LiteralPath $managedPath)) {
            Write-Host "[start-clean-build] Creating $managedPath"
            New-Item -ItemType Directory -Path $managedPath | Out-Null
        }
    }

    $buildPath = Join-Path $projectRoot $BuildDir
    if (Test-Path -LiteralPath $buildPath) {
        $buildResolved = (Resolve-Path -LiteralPath $buildPath).Path
        if (-not $buildResolved.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to delete build path outside project root: $buildResolved"
        }
        Write-Host "[start-clean-build] Removing build directory: $buildResolved"
        Remove-Item -LiteralPath $buildResolved -Recurse -Force
    }

    $codecCmake = Join-Path $projectRoot "managed_components\espressif__esp_codec_dev\CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $codecCmake)) {
        Write-Host "[start-clean-build] Preparing managed components (idf.py -B $BuildDir reconfigure)"
        & idf.py -B $BuildDir reconfigure
        if ($LASTEXITCODE -ne 0) {
            throw "idf.py reconfigure failed with exit code $LASTEXITCODE"
        }
    }

    Ensure-CodecDevCompatibilityPatch -ProjectRootPath $projectRoot

    Write-Host "[start-clean-build] idf.py -B $BuildDir $($BuildArgs -join ' ')"
    & idf.py -B $BuildDir @BuildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
