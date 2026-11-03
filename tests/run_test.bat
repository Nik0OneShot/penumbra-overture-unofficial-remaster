@echo off
rem Builds and runs the offline patch test.
rem   run_test.bat <penumbra_mem.bin> <work dir> [game dir, default ..\game\redist]
rem The dump is a read-only snapshot of the running game's memory (see README).
setlocal
if "%~2"=="" (echo usage: run_test.bat ^<dump^> ^<work dir^> [game dir] & exit /b 2)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
set ROOT=%~dp0..
set GAME=%ROOT%\game\redist
if not "%~3"=="" set GAME=%~f3
set WORK=%~f2
set DUMP=%~f1
if not exist "%WORK%" mkdir "%WORK%"
cl /nologo /Od /Zi /RTC1 /MTd /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /Fo"%WORK%\patch_test.obj" /Fd"%WORK%\patch_test_c.pdb" /Fe"%WORK%\patch_test.exe" "%~dp0patch_test.cpp" /link /DYNAMICBASE /BASE:0x20000000 opengl32.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
copy /y "%ROOT%\dist\SDL_image.dll" "%WORK%\" >nul
if exist "%GAME%\SDL_image_orig.dll" (
    copy /y "%GAME%\SDL_image_orig.dll" "%WORK%\SDL_image_orig.dll" >nul
) else (
    copy /y "%GAME%\SDL_image.dll" "%WORK%\SDL_image_orig.dll" >nul
)
for %%d in (SDL.dll jpeg.dll libpng12.dll libpng13.dll zlib1.dll msvcr71.dll Newton.dll) do copy /y "%GAME%\%%d" "%WORK%\" >nul
copy /y "%GAME%\graphics\menu\menu_background.jpg" "%WORK%\test.jpg" >nul
pushd "%WORK%"
"%WORK%\patch_test.exe" "%DUMP%" test.jpg "%GAME%"
set RC=%ERRORLEVEL%
echo ---- PenumbraUltrawide.log ----
type "%WORK%\PenumbraUltrawide.log"
popd
exit /b %RC%
