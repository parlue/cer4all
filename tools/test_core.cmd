@echo off
setlocal
cd /d "%~dp0.."
if not exist build-tests mkdir build-tests
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /W4 /Iinclude test\core_test.cpp /Fe:build-tests\core_test.exe /Fo:build-tests\core_test.obj
if errorlevel 1 exit /b 1
build-tests\core_test.exe
if errorlevel 1 exit /b 1
if exist "%USERPROFILE%\.platformio\penv\Scripts\python.exe" (
  "%USERPROFILE%\.platformio\penv\Scripts\python.exe" tools\record_test.py
) else (
  python tools\record_test.py
)
