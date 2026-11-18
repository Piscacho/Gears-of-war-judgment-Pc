# Crea la release para GitHub (como la de Ninja Gaiden II):
#   release_staging\Gears of War Judgment\                     carpeta limpia (sin nada del juego ni de pruebas)
#   <Out>\Gears-of-war-judgment-Pc-v<ver>-windows.zip (+ .sha256)  para subir como asset de la release
# Uso (desde la raiz):  powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Ver 1.0.0 [-Out <carpeta>] [-NoBuild]
param([Parameter(Mandatory)][string]$Ver, [string]$Out = "", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$R = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Split-Path $R -Parent }
$name = "Gears of War Judgment"

if (-not $NoBuild) {
    cmd /c "`"$R\tools\build_game.bat`"" | Out-Null; if ($LASTEXITCODE -ne 0) { throw "La compilacion de GOWJ.exe fallo" }
}
$bin = "$R\project\build"

# 1. Carpeta limpia. Sin las imagenes del launcher (launcher\): son de SteamGridDB (terceros); el launcher funciona sin ellas
$stage = "$R\release_staging\$name"
Remove-Item "$R\release_staging" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
Copy-Item "$bin\GOWJ.exe", "$bin\avcodec-62.dll", "$bin\avutil-60.dll", "$bin\swresample-6.dll" $stage
Copy-Item "$R\dist_files\LEEME.txt", "$R\dist_files\gowj.ini", "$R\README.md", "$R\LICENSE", "$R\THIRD-PARTY-NOTICES.txt" $stage
Copy-Item "G:\Guitar Hero Warrior of rock\Recomp\toolchain\ffmpeg\LICENSE.txt" "$stage\licenses\FFmpeg-LGPL-3.0.txt"
Copy-Item "$R\XenonRecomp\LICENSE.md" "$stage\licenses\XenonRecomp-MIT.txt"
# gowj.ini sin rutas personales
$ini = Get-Content "$stage\gowj.ini" -Raw
if ($ini -notmatch '(?m)^iso=\r?$') { throw "dist_files\gowj.ini tiene una ruta al ISO" }
# Nada del juego ni de pruebas en el paquete
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".iso", ".xex", ".xexp", ".bin", ".cso", ".log", ".bmp", ".wav", ".png", ".jpg", ".dat" -or $_.Name -like "TU_*" }
if ($bad) { throw "Archivos que no deben ir en la release: $($bad.FullName -join ', ')" }

# 2. Zip (una carpeta superior con el nombre del juego) + suma SHA-256
$zip = "$Out\Gears-of-war-judgment-Pc-v$Ver-windows.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C "$R\release_staging" $name
if ($LASTEXITCODE -ne 0) { throw "No se pudo crear el zip" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ASCII
Write-Host "Zip:     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Host "SHA-256: $hash"
