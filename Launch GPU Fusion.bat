@echo off
rem Starts GPU Fusion: the packaged release if it exists, otherwise the development build.
rem Any arguments are passed on (e.g. --page=workloads).
setlocal
set "APP=%~dp0release\GPUFusion\GPUFusion.exe"
if not exist "%APP%" set "APP=%~dp0build\release\bin\GPUFusion.exe"
if not exist "%APP%" (
    echo GPU Fusion is not built yet. Run scripts\package.ps1 or scripts\build.ps1 first.
    pause
    exit /b 1
)
start "" "%APP%" %*
