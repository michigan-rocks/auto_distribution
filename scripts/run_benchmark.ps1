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

    # Stop every task-processing service from previous profiles. Monitoring
    # and RabbitMQ stay up, and no queues, volumes or old reports are deleted.
    $allProcessingServices = @('producer', 'producer-baseline', 'dispatcher',
        'round-robin-dispatcher', 'worker1', 'worker2', 'worker3',
        'baseline-worker1', 'baseline-worker2', 'baseline-worker3')
    Invoke-Compose --profile adaptive --profile round-robin --profile baseline stop @allProcessingServices
    Invoke-Compose up -d --wait rabbitmq prometheus grafana
    Write-Host "Starting $experimentId using $poolPath"
    Invoke-Compose --profile $Mode up -d --no-deps --force-recreate @services
    & docker compose --profile $Mode wait $producerService
    $producerExitCode = $LASTEXITCODE
    Invoke-Compose --profile $Mode logs --tail 10 --no-log-prefix $producerService

    $reportPath = Join-Path $settings.RESULTS_DIR ($experimentId + '.json')
    if ($producerExitCode -ne 0) {
        throw "Producer exited with code $producerExitCode; inspect its logs and $reportPath"
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
