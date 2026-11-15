param(
    [Parameter(Mandatory)][string]$Ghidra,
    [string]$ProjectDirectory = (Split-Path -Parent $PSScriptRoot),
    [string]$ProjectName = 'Sekiro_Analysis',
    [string[]]$Focus = @('14235a82c', '14235a120', '141b69610', '141b6ff70',
                        '141de7760', '141de7c70', '141de85b0', '1411e42c0', '141b5e9a0', '1411e40d0', '1406e1be0', '141de3b90',
                        'data:1429934d0', 'data:142993508', 'data:142993540',
                        'data:142993578', 'data:1429935b0', 'data:142994360')
)
$ErrorActionPreference = 'Stop'
$analysis = Join-Path $PSScriptRoot 'reverse-engineering'
$outputDir = Join-Path $analysis 'reports/generated/ghidra'
New-Item -ItemType Directory -Force $outputDir | Out-Null
$marker = Join-Path $outputDir 'complete.txt'
if (Test-Path -LiteralPath $marker) {
    Remove-Item -LiteralPath $marker
}
& (Join-Path $Ghidra 'support/analyzeHeadless.bat') $ProjectDirectory $ProjectName `
    -process sekiro.exe -noanalysis -readOnly -scriptPath (Join-Path $analysis 'scripts') `
    -postScript ExportSekiroEvidence.java $outputDir @Focus -log (Join-Path $outputDir 'headless.log') `
    -scriptlog (Join-Path $outputDir 'script.log')
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $marker)) {
    throw 'Ghidra export failed. Check the headless log.'
}
