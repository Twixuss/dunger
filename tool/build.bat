@echo off

set cFlags=/nologo /link

set bindir=..\bin
set srcdir=..\src\

if not exist %bindir% mkdir %bindir%
pushd %bindir%

set runtime=/MD

if "%1" equ "r" (
    set cFlags=/O2 /GL /D"BUILD_DEBUG=0" %cFlags% /LTCG
) else (
    set runtime=%runtime%d
    set cFlags=/Od /D"BUILD_DEBUG=1" %cFlags%
)

set arch=%VSCMD_ARG_TGT_ARCH%

set deps=user32.lib d3d11.lib d3dcompiler.lib dxgi.lib winmm.lib opengl32.lib gdi32.lib shell32.lib
set cFlags=%runtime% /Z7 /FC /Oi /EHa /fp:fast /std:c++latest %cFlags% /LIBPATH:"../dep/Microsoft DirectX SDK/Lib/%arch%/" %deps% /incremental:no

::set cFlags=/arch:AVX /favor:INTEL64 %cFlags%

cl %srcdir%main.cpp %cFlags% /out:game.exe
if %errorlevel% neq 0 goto fail

::cl %srcdir%server.cpp %cFlags% /out:server.exe
::if %errorlevel% neq 0 goto fail

echo [32mCompilation succeeded[0m
goto end
:fail
echo [31mCompilation failed[0m
:end
popd