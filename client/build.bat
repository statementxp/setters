@echo off
chcp 65001 >nul
setlocal

echo Сборка Secure Client...
echo.

REM Проверка структуры
if not exist src\main.c (
    echo Ошибка: запустите из корня проекта где есть папка src\
    pause
    exit /b 1
)

REM Сборка
gcc -O2 -s -static ^
  -Ithird_party ^
  -DUNICODE -D_UNICODE ^
  src/main.c ^
  src/manifest.c ^
  src/downloader.c ^
  src/ui.c ^
  third_party/cJSON.c ^
  -o secure-client.exe ^
  -lwininet -lgdi32 -luser32 -lkernel32 -lbcrypt -lshlwapi -lmsvcrt

if errorlevel 1 (
    echo.
    echo ОШИБКА СБОРКИ
    pause
    exit /b 1
)

echo.
echo Успешно собрано: secure-client.exe (%~z0 байт)
echo.
echo Запуск...
secure-client.exe