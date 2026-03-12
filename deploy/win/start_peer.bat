@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1

:: ============================================================
::  p2p_peer.exe one-click launcher
::  Connects to k3s server: 106.54.30.119
:: ============================================================

cd /d "%~dp0"

set SIGNALING=106.54.30.119:30443
set STUN=106.54.30.119:3478
set PEER_ID=sub1
set ROOM=
set ADMIN_SECRET=eLTGSBSmlCZqar7lwkf4GFje
set NO_VIDEO=
set NO_AUDIO=

:: Parse command-line overrides
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--room"      ( set ROOM=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--peer-id"   ( set PEER_ID=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--no-video"  ( set NO_VIDEO=--no-video& shift & goto parse_args )
if /i "%~1"=="--no-audio"  ( set NO_AUDIO=--no-audio& shift & goto parse_args )
shift
goto parse_args
:args_done

:: If room not specified, prompt user
if "%ROOM%"=="" (
    set /p ROOM="Enter room name to join: "
)
if "%ROOM%"=="" (
    echo [launcher] ERROR: Room name is required for peer.
    pause
    exit /b 1
)

echo ============================================================
echo  P2P Peer ^(Subscriber^) Launcher
echo ============================================================
echo  Signaling : %SIGNALING%
echo  STUN      : %STUN%
echo  Room      : %ROOM%
echo  Peer ID   : %PEER_ID%
echo ============================================================

:: Request JWT token from signaling server using curl.exe (Windows built-in)
echo [launcher] Requesting JWT token...
set TOKEN_URL=https://%SIGNALING%/v1/token?peer_id=%PEER_ID%

for /f "usebackq delims=" %%T in (`curl.exe -sk -H "Authorization: Bearer %ADMIN_SECRET%" "%TOKEN_URL%" 2^>nul`) do (
    set RAW_RESP=%%T
)

:: Extract token from JSON response: {"token":"xxx"}
if defined RAW_RESP (
    for /f "tokens=2 delims=:}" %%A in ("!RAW_RESP!") do (
        set TOKEN=%%~A
    )
)
if defined TOKEN set TOKEN=!TOKEN:"=!

if "!TOKEN!"=="" (
    echo [launcher] WARNING: Failed to get token. Trying without auth...
    set TOKEN_ARG=
) else (
    echo [launcher] Token acquired: !TOKEN:~0,20!...
    set TOKEN_ARG=--token !TOKEN!
)

echo [launcher] Starting p2p_peer.exe ...
echo.

p2p_peer.exe ^
  --signaling %SIGNALING% ^
  --stun %STUN% ^
  --room %ROOM% ^
  --peer-id %PEER_ID% ^
  %NO_VIDEO% %NO_AUDIO% ^
  !TOKEN_ARG!

echo.
echo [launcher] p2p_peer.exe exited.
pause
