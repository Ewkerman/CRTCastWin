# Builds CRTCast for Windows: the native core (CMake + Ninja + MSVC) and the WPF app (.NET 8).
#   powershell -ExecutionPolicy Bypass -File CRTCastWin\build.ps1 [-Config Release|Debug] [-CoreOnly]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$CoreOnly,
    [switch]$NoRio   # groovymister with plain sockets instead of Registered I/O (see core/CMakeLists.txt)
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Find the newest Visual Studio (or Build Tools) with the C++ x64 compiler.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools (x64) not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# vcpkg with OpenSSL, libplist and pthreads for the AirPlay receiver:
#   vcpkg install openssl:x64-windows-static libplist:x64-windows-static pthreads:x64-windows-static
# Use the first vcpkg (VCPKG_ROOT, then %USERPROFILE%\vcpkg) that has those static packages installed.
$candidates = @($env:VCPKG_ROOT, (Join-Path $env:USERPROFILE 'vcpkg')) | Where-Object { $_ }
$vcpkg = $candidates | Where-Object { Test-Path (Join-Path $_ 'installed\x64-windows-static\lib\libcrypto.lib') } | Select-Object -First 1
if (-not $vcpkg) { throw "No vcpkg with x64-windows-static OpenSSL/libplist/pthreads found in: $($candidates -join ', ')" }
$toolchain = Join-Path $vcpkg 'scripts\buildsystems\vcpkg.cmake'
$env:VCPKG_ROOT = $vcpkg   # for this build only
Write-Host "vcpkg: $vcpkg"

$core = Join-Path $root 'core'
$buildDir = Join-Path $core "build\$Config"
cmd /c "`"$vcvars`" >nul && cmake -S `"$core`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCRTCAST_GROOVY_RIO=$(if ($NoRio) {'OFF'} else {'ON'}) && cmake --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw 'Native core build failed.' }
Write-Host "Core: $buildDir\bin"

if ($CoreOnly) { return }
dotnet build (Join-Path $root 'app\CRTCastWin.csproj') -c $Config -p:CoreBin="$buildDir\bin"
if ($LASTEXITCODE -ne 0) { throw 'App build failed.' }
Write-Host "App: $root\app\bin\$Config\net8.0-windows\win-x64\CRTCastWin.exe"
