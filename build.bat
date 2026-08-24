@echo off
title anx1ous Launcher Build
echo ===================================================
echo   Building anx1ous Launcher (v1.0.3)
echo ===================================================
echo.
echo [1/2] Compiling Windows Resources...
windres.exe app.rc -o res.o
if %errorlevel% neq 0 (
    echo [ERROR] Resource compilation failed.
    pause
    exit /b %errorlevel%
)

echo [2/2] Compiling C++ source...
g++.exe -o anx1ous.exe main.cpp res.o ^
  -std=c++17 -O2 -m64 -mwindows -municode ^
  -DUNICODE -D_UNICODE ^
  -static ^
  -Wl,--allow-multiple-definition ^
  -lkernel32 -luser32 -lgdi32 -ladvapi32 ^
  -ldwmapi -luxtheme -lcomctl32 ^
  -lurlmon -lwininet -lgdiplus -lcrypt32 -lole32 -loleaut32 -luuid

if %errorlevel% equ 0 (
    echo.
    echo ===================================================
    echo  [SUCCESS] anx1ous.exe created!
    echo ===================================================
) else (
    echo.
    echo [ERROR] Compilation failed.
)
pause
