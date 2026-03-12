@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1

:: ============================================================
::  p2p_client.exe one-click launcher
::  Connects to k3s server: 106.54.30.119
:: ============================================================

cd /d "%~dp0"

set SIGNALING=106.54.30.119:30443
set STUN=106.54.30.119:3478
set PEER_ID=pub1
set ROOM=
set AUDIO_DEV=@device_cm_{33D9A762-90C8-11D0-BD43-00A0C911CE86}\wave_{22546F77-F90D-4471-8D1E-149C45D8BBF4}
set VIDEO_DEV=Integrated Camera
set ADMIN_SECRET=eLTGSBSmlCZqar7lwkf4GFje
set NO_VIDEO=
set NO_AUDIO=

:: Parse command-line overrides
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--room"      ( set ROOM=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--peer-id"   ( set PEER_ID=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--audio-dev" ( set AUDIO_DEV=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--video-dev" ( set VIDEO_DEV=%~2& shift & shift & goto parse_args )
if /i "%~1"=="--no-video"  ( set NO_VIDEO=--no-video& shift & goto parse_args )
if /i "%~1"=="--no-audio"  ( set NO_AUDIO=--no-audio& shift & goto parse_args )
shift
goto parse_args
:args_done

:: Auto-generate room name if not specified
if "%ROOM%"=="" (
    set /a ROOM_NUM=%RANDOM% * 10 + %RANDOM% %% 10
    set ROOM=room!ROOM_NUM!
)

echo ============================================================
echo  P2P Client ^(Publisher^) Launcher
echo ============================================================
echo  Signaling : %SIGNALING%
echo  STUN      : %STUN%
echo  Room      : %ROOM%
echo  Peer ID   : %PEER_ID%
echo  Video Dev : %VIDEO_DEV%
echo  Audio Dev : %AUDIO_DEV%
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
:: Remove surrounding quotes if any
if defined TOKEN set TOKEN=!TOKEN:"=!

if "!TOKEN!"=="" (
    echo [launcher] WARNING: Failed to get token. Trying without auth...
    set TOKEN_ARG=
) else (
    echo [launcher] Token acquired: !TOKEN:~0,20!...
    set TOKEN_ARG=-T !TOKEN!
)

echo [launcher] Starting p2p_client.exe ...
echo.

p2p_client.exe ^
  --signaling %SIGNALING% ^
  --stun %STUN% ^
  --ssl-cert server.crt ^
  --ssl-key server.key ^
  --room %ROOM% ^
  --peer-id %PEER_ID% ^
  --video-dev "%VIDEO_DEV%" ^
  --audio-dev "%AUDIO_DEV%" ^
  %NO_VIDEO% %NO_AUDIO% ^
  !TOKEN_ARG!

echo.
echo [launcher] p2p_client.exe exited.
pause
