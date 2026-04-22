param(
    [string]$BuildDir = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $hostName = $env:COMPUTERNAME
    if ([string]::IsNullOrWhiteSpace($hostName)) {
        $hostName = "default"
    }
    $BuildDir = "build_$hostName"
}

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
    $IdfArgs = @("build")
}

Write-Host "[portable-build] Build directory: $BuildDir"
Write-Host "[portable-build] idf.py -B $BuildDir $($IdfArgs -join ' ')"

& idf.py -B $BuildDir @IdfArgs
