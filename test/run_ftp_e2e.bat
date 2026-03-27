@echo off
echo ==================== FTP E2E Test ====================
echo.
echo Step 1: Starting FTP Server...
start /MIN "FTP Server" e:\test\server-lib\build2\test\ftp_e2e_server.exe
echo Server started, waiting for initialization...
timeout /t 3 /nobreak > nul
echo.
echo Step 2: Running FTP Client tests...
e:\test\server-lib\build2\test\ftp_e2e_client.exe
set CLIENT_EXIT=%ERRORLEVEL%
echo.
echo Client exited with code: %CLIENT_EXIT%
echo.
echo Step 3: Stopping server...
taskkill /F /IM ftp_e2e_server.exe > nul 2>&1
echo Server stopped
echo.
if %CLIENT_EXIT% EQU 0 (
    echo ==================== TEST PASSED ====================
) else (
    echo ==================== TEST FAILED ====================
)
