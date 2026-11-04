param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('adaptive', 'round-robin', 'baseline')]
    [string]$Mode,
    [string]$Workload = 'workloads/default.jsonl',
    [ValidateRange(1, 86400)]
    [int]$TimeoutSeconds = 3600,
    [ValidateRange(0, 60000)]
    [int]$IntervalMs = 0,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
Push-Location $projectDirectory

function Invoke-Compose {
    & docker compose @args
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed with exit code $LASTEXITCODE"
    }
}

$savedEnvironment = @{}
try {
    $poolPath = (Resolve-Path -LiteralPath $Workload).Path
    if (-not (Test-Path -LiteralPath $poolPath -PathType Leaf)) {
        throw "Workload must be a file: $poolPath"
    }
    $experimentId = $Mode + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' +
                    ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $settings = @{
        EXPERIMENT_ID = $experimentId
        SCHEDULER_MODE = $Mode
        WORKLOAD_DIR = (Split-Path -Parent $poolPath)
        PRODUCER_WORKLOAD = '/workloads/' + (Split-Path -Leaf $poolPath)
        RESULTS_DIR = (Join-Path $projectDirectory 'results')
        BENCHMARK_TIMEOUT_SECONDS = [string]$TimeoutSeconds
        PRODUCER_INTERVAL_MS = [string]$IntervalMs
        WORKER_SEND_FULL_RESULT = 'false'
    }
    foreach ($name in $settings.Keys) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process')
    }

    $producerService = if ($Mode -eq 'baseline') { 'producer-baseline' } else { 'producer' }
    $services = if ($Mode -eq 'baseline') {
        @('baseline-worker1', 'baseline-worker2', 'baseline-worker3', $producerService)
    } else {
        $dispatcherService = if ($Mode -eq 'adaptive') { 'dispatcher' } else { 'round-robin-dispatcher' }
        @('worker1', 'worker2', 'worker3', $dispatcherService, $producerService)
    }
    if (-not $NoBuild) {
        Invoke-Compose --profile $Mode build @services
    }

    # Runtime databases live in container layers and disappear on down.
    # The workload and JSON reports are host bind mounts and survive the reset.
    Write-Host 'Resetting containers and transient broker/monitoring data'
    Invoke-Compose --profile adaptive --profile round-robin --profile baseline down --remove-orphans
    Invoke-Compose up -d --wait rabbitmq prometheus grafana
    Write-Host "Starting $experimentId using $poolPath"
    Invoke-Compose --profile $Mode up -d --no-deps --force-recreate @services
    # Include exited containers: producer can finish before we reach this line.
    $producerContainerIds = @(& docker compose --profile $Mode ps --all --quiet $producerService)
    if ($LASTEXITCODE -ne 0 -or $producerContainerIds.Count -ne 1) {
        throw "Cannot find exactly one container for $producerService"
    }
    $producerExitStatus = & docker wait $producerContainerIds[0]
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot wait for producer container $($producerContainerIds[0])"
    }
    $producerExitCode = 0
    if (-not [int]::TryParse([string]$producerExitStatus, [ref]$producerExitCode)) {
        throw "Unexpected producer exit status: $producerExitStatus"
    }
    Invoke-Compose --profile $Mode logs --tail 10 --no-log-prefix $producerService

    $reportPath = Join-Path $settings.RESULTS_DIR ($experimentId + '.json')
    if ($producerExitCode -ne 0) {
        if (Test-Path -LiteralPath $reportPath) {
            throw "Producer exited with code $producerExitCode; inspect its logs and $reportPath"
        }
        throw "Producer exited with code $producerExitCode before creating a report; inspect the logs above"
    }
    $report = Get-Content -Raw -LiteralPath $reportPath | ConvertFrom-Json
    if ($report.status -ne 'completed' -or $report.completed -ne $report.expected) {
        throw "Incomplete experiment; inspect $reportPath"
    }
    Write-Host "Mode: $($report.mode); tasks: $($report.completed); seconds: $($report.elapsed_seconds)"
    Write-Host "Workers: $($report.workers | ConvertTo-Json -Compress)"
    Write-Host "Report: $reportPath"
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
