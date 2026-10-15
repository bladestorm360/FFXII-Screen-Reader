@echo off
setlocal
pushd "%~dp0.."
call "..\..\..\dev_env.bat"
if not defined CMAKE_EXE goto Failed
call "%VCVARSALL%" x64 >nul
if errorlevel 1 goto Failed
for %%I in ("%CMAKE_EXE%") do set "FFXII_CTEST=%%~dpIctest.exe"
if not exist build mkdir build
"%CMAKE_EXE%" -S tests -B build\core-tests-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release >build\core-tests-configure.log 2>&1
if errorlevel 1 goto Failed
"%CMAKE_EXE%" --build build\core-tests-ninja >build\core-tests-build.log 2>&1
if errorlevel 1 goto Failed
"%FFXII_CTEST%" --test-dir build\core-tests-ninja --output-on-failure >build\core-tests-results.log 2>&1
if errorlevel 1 goto Failed
echo Core and speech regression tests passed. Logs are in build\core-tests-*.log.
popd
exit /b 0
:Failed
echo Regression checks failed. See build\core-tests-configure.log, core-tests-build.log or core-tests-results.log.
popd
exit /b 1
