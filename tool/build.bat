@echo off

set cFlags=
set lFlags=

set bindir=..\bin
set srcdir=..\src\

if not exist %bindir% mkdir %bindir%
pushd %bindir%

set runtime=/MD

if "%1" equ "r" (
    set cFlags=/O2 /GL /D"BUILD_DEBUG=0"
    set lFlags=/LTCG
) else (
    set runtime=%runtime%d
    set cFlags=/Od /D"BUILD_DEBUG=1"
)

set cFlags=%cFlags% /Z7 /FC /Oi /EHa /fp:fast /std:c++latest
set lFlags=%lFlags% /incremental:no
set deps=user32.lib d3d11.lib d3dcompiler.lib dxgi.lib winmm.lib opengl32.lib gdi32.lib shell32.lib

set cFlags=%cFlags% /favor:INTEL64

cl %srcdir%main.cpp %runtime% %cFlags% /nologo /link %lFlags% /out:game.exe %deps%
if %errorlevel% neq 0 goto fail

cl %srcdir%server.cpp %runtime% %cFlags% /nologo /link %lFlags% /out:server.exe %deps%
if %errorlevel% neq 0 goto fail

echo [32mCompilation succeeded[0m
goto end
:fail
echo [31mCompilation failed[0m
:end
popd