# Starts Ollama if it isn't running, then downloads SynthMatch's assistant model.
# The installer runs this in the background as the person installing. It is
# safe to run again by hand; Ollama skips what it already has.

param([string]$Model = "qwen3:1.7b")

$folder = Join-Path $env:LOCALAPPDATA "Programs\Ollama"
$ollama = Join-Path $folder "ollama.exe"
$app = Join-Path $folder "ollama app.exe"
if (-not (Test-Path $ollama)) { exit 0 }

function Test-Server {
    try {
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:11434/api/version" | Out-Null
        return $true
    } catch {
        return $false
    }
}

if (-not (Test-Server)) {
    if (Test-Path $app) {
        Start-Process -FilePath $app
    } else {
        Start-Process -FilePath $ollama -ArgumentList "serve" -WindowStyle Hidden
    }
    for ($i = 0; $i -lt 90 -and -not (Test-Server); $i++) { Start-Sleep -Seconds 1 }
}

& $ollama pull $Model
