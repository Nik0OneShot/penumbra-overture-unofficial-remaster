@echo off
rem Builds dist\SDL_image.dll (32-bit) with the Visual Studio 2022 Build Tools.
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
call "%VCVARS%" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\dist mkdir ..\dist
if not exist obj mkdir obj
cl /nologo /O2 /MT /W3 /EHsc /LD /Foobj\ PenumbraUltrawide.cpp HighFramerate.cpp ^
   /link /DEF:SDL_image.def /OUT:..\dist\SDL_image.dll /IMPLIB:obj\SDL_image.lib opengl32.lib user32.lib || exit /b 1
copy /y PenumbraUltrawide.ini ..\dist\ >nul
echo Built %~dp0..\dist\SDL_image.dll
