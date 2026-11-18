@echo off
setlocal

if "%~2"=="" (
  echo Usage: compile_papyrus.bat "Starfield folder" "OSF Settings Scripts\Source folder"
  exit /b 2
)

set "STARFIELD=%~1"
set "OSF_SETTINGS_SOURCE=%~2"
set "BASE=%STARFIELD%\Tools\ContentResources\Scripts\Source"
set "COMPILER=%STARFIELD%\Tools\Papyrus Compiler\PapyrusCompiler.exe"
set "FLAGS=%BASE%\Starfield_Papyrus_Flags.flg"
set "SOURCE=%~dp0scripts\Source"
set "OUTPUT=%~dp0dist\Data\Scripts"

if not exist "%COMPILER%" (
  echo Papyrus compiler not found: "%COMPILER%"
  echo Install the Starfield Creation Kit and check the Starfield folder.
  exit /b 1
)
if not exist "%FLAGS%" (
  echo Flags file not found: "%FLAGS%"
  echo Extract Tools\ContentResources.zip and point BASE at its Scripts\Source folder.
  exit /b 1
)
if not exist "%OSF_SETTINGS_SOURCE%\OSFSettings.psc" (
  echo OSFSettings.psc not found in "%OSF_SETTINGS_SOURCE%"
  exit /b 1
)

if not exist "%OUTPUT%" mkdir "%OUTPUT%"
"%COMPILER%" "Liftoff_OSF.psc" -f="%FLAGS%" -i="%SOURCE%;%OSF_SETTINGS_SOURCE%;%BASE%" -o="%OUTPUT%"
exit /b %errorlevel%
