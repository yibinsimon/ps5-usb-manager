@echo off
rem ============================================================
rem  usbmanage.elf — Windows 编译入口（cmd.exe）
rem
rem  行为：
rem    1) 若 PATH 中有 bash（Git Bash / MSYS2），直接转调 build.sh
rem       —— 这是本机实测通过的主路径。
rem    2) 否则退化为纯 cmd 直接调 clang，参数与 build.sh 等价
rem       （该分支未在本机实测，cmd.exe 受环境限制）。
rem
rem  前置：宿主机有 clang + ld.lld（llvm-mingw 一个包即可）。
rem  环境变量（不设则用下面的默认值）：
rem      PS5_PAYLOAD_SDK   指向 ps5-payload-sdk 根目录
rem      PS5_CLANG         指向 clang.exe
rem
rem  用法：在 device\ps5-usbmanage 目录下执行  win\build.cmd
rem  产物：usbmanage.elf
rem
rem  注意：不要改用 SDK 自带的 win\prospero-clang.cmd。
rem        它漏了 clang>=20 的 crt1.o 去重逻辑，会报
rem        "duplicate symbol: payload_exit" 链接失败。
rem ============================================================
setlocal
rem 使中文输出正常显示（脚本本身为 UTF-8 无 BOM）
chcp 65001 >nul

pushd "%~dp0.."

rem ---- 路径 1：有 bash 就转调 build.sh ----
where bash >nul 2>nul
if errorlevel 1 goto direct
bash build.sh
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

:direct
rem ---- 路径 2：纯 cmd ----
rem 不写死任何绝对路径。两个变量都从环境变量取；缺了就直接报错并给出设置方法。
if "%PS5_PAYLOAD_SDK%"=="" (
  echo [error] 未设置 PS5_PAYLOAD_SDK
  echo         请指向 ps5-payload-sdk 根目录（解压后的那个文件夹），例如：
  echo           set PS5_PAYLOAD_SDK=C:\ps5-payload-sdk
  echo         获取：https://github.com/ps5-payload-dev/sdk/releases/latest
  popd ^& exit /b 1
)
if "%PS5_CLANG%"=="" (
  echo [error] 未设置 PS5_CLANG
  echo         请指向 clang.exe（llvm-mingw 解压后的 bin 目录里），例如：
  echo           set PS5_CLANG=C:\llvm-mingw\bin\clang.exe
  popd ^& exit /b 1
)

if not exist "%PS5_PAYLOAD_SDK%\target\include" (
  echo [error] 这个目录不像 SDK 根目录: %PS5_PAYLOAD_SDK%
  echo         应含 target\include 子目录
  popd ^& exit /b 1
)
if not exist "%PS5_CLANG%" (
  echo [error] 找不到 clang: %PS5_CLANG%
  popd ^& exit /b 1
)

for %%I in ("%PS5_CLANG%") do set "CLANG_DIR=%%~dpI"
if "%CLANG_DIR:~-1%"=="\" set "CLANG_DIR=%CLANG_DIR:~0,-1%"

rem PATH 需同时含 clang 目录（供 ld.lld）与 SDK\win（供 prospero-lld.exe）
set "PATH=%CLANG_DIR%;%PS5_PAYLOAD_SDK%\win;%PATH%"
rem 让驱动把 --sysroot 指向 SDK 自身（对齐官方 wrapper）
set "SCE_PROSPERO_SDK_DIR=%PS5_PAYLOAD_SDK%"

echo [build] clang : %PS5_CLANG%
echo [build] sdk   : %PS5_PAYLOAD_SDK%

"%PS5_CLANG%" ^
  --start-no-unused-arguments ^
  -target x86_64-sie-ps5 ^
  -fvisibility-nodllstorageclass=default ^
  -isysroot "%PS5_PAYLOAD_SDK%" ^
  -isystem "%PS5_PAYLOAD_SDK%\target\include" ^
  -L "%PS5_PAYLOAD_SDK%\target\lib" ^
  -L "%PS5_PAYLOAD_SDK%\target\user\homebrew\lib" ^
  -fno-stack-protector -fno-plt -femulated-tls ^
  -Os -Wall -Wextra ^
  -lc -lkernel_sys ^
  --end-no-unused-arguments ^
  usbmanage.c -o usbmanage.elf ^
  --start-no-unused-arguments ^
  --sysroot "%PS5_PAYLOAD_SDK%" ^
  -lSceLibcInternal -lSceNet ^
  --end-no-unused-arguments

if errorlevel 1 (
  echo [error] 编译失败
  popd & exit /b 1
)

echo [ok] usbmanage.elf
popd
endlocal
