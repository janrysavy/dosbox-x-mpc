[CmdletBinding()]
param(
    [string]$DosboxExecutable = '',
    [string]$PythonExecutable = 'python',
    [int]$RunCount = 3
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (!$Condition) {
        throw $Message
    }
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$defaultDosboxExecutable = Join-Path $repositoryRoot 'bin\x64\Agent Debug SDL2\dosbox-x.exe'
if (!$DosboxExecutable) {
    $DosboxExecutable = $defaultDosboxExecutable
}
$dosboxPath = (Resolve-Path -LiteralPath $DosboxExecutable).Path
$e2eScript = Join-Path $repositoryRoot 'client\python\tests\test_e2e.py'
Assert-Condition (Test-Path -LiteralPath $dosboxPath -PathType Leaf) "DOSBox-X executable was not found: $dosboxPath"
Assert-Condition (Test-Path -LiteralPath $e2eScript -PathType Leaf) "E2E script was not found: $e2eScript"

$runRoot = Join-Path $PSScriptRoot ('e2e-clean-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null

for ($index = 1; $index -le $RunCount; ++$index) {
    $runtimeDirectory = Join-Path $runRoot ("runtime-$index")
    $configPath = Join-Path $runRoot ("agent-e$index.env")
    $dosboxConfigPath = [System.IO.Path]::ChangeExtension($configPath, '.conf')
    New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null

    & (Join-Path $PSScriptRoot 'build_fixture.ps1') -RuntimeDirectory $runtimeDirectory
    Copy-Item -LiteralPath $dosboxPath -Destination (Join-Path $runtimeDirectory 'dosbox-x.exe') -Force

    $config = @(
        'transport=named_pipe',
        '\\.\pipe\dosbox-agent-test',
        ('dosbox_executable={0}' -f (Join-Path $runtimeDirectory 'dosbox-x.exe')),
        ('dosbox_workdir={0}' -f $runtimeDirectory),
        'profile=test',
        'request_timeout_ms=5000',
        'max_message_bytes=1048576',
        'max_memory_read_bytes=65536',
        'max_trace_events=10000'
    )
    $config[1] = 'endpoint=' + $config[1]
    [System.IO.File]::WriteAllText($configPath, ($config -join [Environment]::NewLine) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        $dosboxConfigPath,
        "[sdl]$([Environment]::NewLine)fullscreen=false$([Environment]::NewLine)[cpu]$([Environment]::NewLine)core=normal$([Environment]::NewLine)[autoexec]$([Environment]::NewLine)path ;$([Environment]::NewLine)",
        [System.Text.UTF8Encoding]::new($false)
    )

    & $PythonExecutable $e2eScript --config $configPath
    if ($LASTEXITCODE -ne 0) {
        throw "E2E run $index failed"
    }
}

Write-Output "RPC-E04 passed: $RunCount independent clean E2E run(s) completed under $runRoot."
