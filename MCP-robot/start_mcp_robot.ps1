$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$pythonExe = Join-Path $projectRoot "venv\\Scripts\\python.exe"
$mainFile = Join-Path $projectRoot "main.py"

if (-not (Test-Path $pythonExe)) {
    throw "未找到解释器: $pythonExe"
}

$existing = Get-NetTCPConnection -LocalPort 8080 -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty OwningProcess -Unique

foreach ($procId in $existing) {
    if ($procId) {
        try {
            Stop-Process -Id $procId -Force -ErrorAction Stop
            Write-Host "已停止占用 8080 的进程: $procId"
        } catch {
            Write-Warning "停止进程失败: $procId - $($_.Exception.Message)"
        }
    }
}

Write-Host "使用解释器启动 MCP-robot: $pythonExe"
& $pythonExe $mainFile
