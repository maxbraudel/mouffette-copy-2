param(
    [string]$WorkspaceRoot = (Split-Path -Parent $PSScriptRoot)
)

$matrixPath = Join-Path $WorkspaceRoot "docs\QUICK_CANVAS_INPUT_TEST_MATRIX.md"
$budgetPath = Join-Path $WorkspaceRoot "tools\input_perf_budget.json"

Write-Host "Quick Canvas Input Smoke Check" -ForegroundColor Cyan
Write-Host "Matrix: $matrixPath"
Write-Host "Perf Budget: $budgetPath"
Write-Host ""
Write-Host "Manual execution required for GUI interaction cases." -ForegroundColor Yellow
Write-Host "Use this checklist before merge:" -ForegroundColor Yellow
Write-Host "  1) Build succeeds"
Write-Host "  2) Run all functional cases"
Write-Host "  3) Run robustness edge cases"
Write-Host "  4) Confirm perf budget expectations"

if ((Test-Path $matrixPath) -and (Test-Path $budgetPath)) {
    Write-Host "Checklist artifacts present." -ForegroundColor Green
    exit 0
}

Write-Error "Missing checklist artifacts."
exit 1
