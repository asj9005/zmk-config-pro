param(
    [ValidateRange(0.001, 72)][double]$Hours = 24,
    [string[]]$Port = @(),
    [string]$Output = (Join-Path $PSScriptRoot 'captures')
)
$ErrorActionPreference = 'Stop'
$captureScript = Join-Path $PSScriptRoot 'collect_field_diagnostics.py'
$captureArgs = @($captureScript, '--hours', $Hours.ToString([Globalization.CultureInfo]::InvariantCulture), '--output', $Output)
foreach ($consolePort in $Port) { $captureArgs += @('--port', $consolePort) }
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 @captureArgs
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python @captureArgs
} else {
    throw 'Python 3 was not found. Use the existing Python installation; the firmware SDK is not required.'
}
exit $LASTEXITCODE
