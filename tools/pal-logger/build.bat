@echo off
rem Builds the x86 pal.dll logging proxy, the fake pal_real.dll and the test, then runs the test. Needs VS 2022 Build Tools (C++).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist out mkdir out
cl /nologo /O2 /EHsc /std:c++17 /W4 /MT /LD /I include pal_logger.cpp /link /DEF:pal.def /OUT:out\pal.dll /MACHINE:X86 || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /W4 /MT /LD fake_pal.cpp /link /DEF:fake_pal.def /OUT:out\pal_real.dll /MACHINE:X86 || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /W4 /MT test_logger.cpp /Fe:out\test_logger.exe /link /MACHINE:X86 || exit /b 1
del /q out\pal_log_*.txt 2>nul
pushd out
test_logger.exe
set rc=%errorlevel%
popd
dumpbin /nologo /exports out\pal.dll | findstr /r "Exit GetDeviceInfo GetNumDevices Init ReadOp SelectDevice SetBufferSize SetCallback WriteOp"
exit /b %rc%
