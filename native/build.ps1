# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the native loader on Windows, into work/native/windows/mensis-loader.exe.
#
#     pwsh native/build.ps1
#
# Needs the Visual Studio 2022 Build Tools (or Visual Studio) with the "Desktop development with
# C++" workload and its "C++ Clang tools for Windows" component, CMake, and the Vulkan SDK
# (vulkan.lunarg.com; CMake finds it through the VULKAN_SDK its installer sets). Finds the
# compiler with vswhere and sets up its environment, so it runs from any shell.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools not found (no vswhere.exe)' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'no Visual Studio installation with the C++ tools found' }
$llvm = Join-Path $vs 'VC\Tools\Llvm\x64\bin'
if (-not (Test-Path (Join-Path $llvm 'clang++.exe'))) {
    throw "clang not found in ${llvm}: add the 'C++ Clang tools for Windows' component"
}

# the compiler's environment: the Windows SDK and C++ library paths
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$build = Join-Path $root 'work\native\windows'
cmake -S (Join-Path $root 'native') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_COMPILER=$llvm\clang++.exe" "-DCMAKE_ASM_COMPILER=$llvm\clang.exe"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
exit $LASTEXITCODE
