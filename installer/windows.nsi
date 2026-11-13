Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma
CRCCheck on

!include "MUI2.nsh"

!ifndef APP_VERSION_DISPLAY
  !define APP_VERSION_DISPLAY "4.9.0-custom"
!endif
!ifndef APP_VERSION_NUMERIC
  !define APP_VERSION_NUMERIC "4.9.0.0"
!endif

!define APP_NAME "SmallTV"
!define APP_PUBLISHER "JPHultra"
!define APP_EXE "smalltv-emulator.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SmallTV"
!define APP_KEY "Software\SmallTV"

Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\SmallTV"
InstallDirRegKey HKCU "${APP_KEY}" "InstallLocation"
BrandingText "${APP_NAME} ${APP_VERSION_DISPLAY}"

!define MUI_ABORTWARNING
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\app\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchInstalledApp
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!define MUI_WELCOMEPAGE_TITLE "${APP_NAME} ${APP_VERSION_DISPLAY}"
!define MUI_WELCOMEPAGE_TEXT "Install the ${APP_NAME} desktop app for Windows."

VIProductVersion "${APP_VERSION_NUMERIC}"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=1033 "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION_DISPLAY}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION_DISPLAY}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "WTFPL"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${APP_LICENSE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function LaunchInstalledApp
  SetOutPath "$INSTDIR\app"
  Exec '"$INSTDIR\app\${APP_EXE}"'
FunctionEnd

Section "!${APP_NAME}" SEC_CORE
  SectionIn RO
  SetOutPath "$INSTDIR\app"
  File /r "${PAYLOAD}\*"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS"
  SetOutPath "$INSTDIR\app"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\app\${APP_EXE}" "" "$INSTDIR\app\${APP_EXE}" 0
  Delete "$DESKTOP\${APP_NAME}.lnk"

  WriteRegStr HKCU "${APP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION_DISPLAY}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut" SEC_DESKTOP
  SetOutPath "$INSTDIR\app"
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\app\${APP_EXE}" "" "$INSTDIR\app\${APP_EXE}" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE} "Install the SmallTV desktop app."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} "Create a SmallTV shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  nsExec::ExecToLog 'schtasks /End /TN "SmallTVUsageWorker"'
  Pop $0
  nsExec::ExecToLog 'schtasks /Delete /TN "SmallTVUsageWorker" /F'
  Pop $0
  nsExec::ExecToLog 'schtasks /End /TN "SmallTVUsage"'
  Pop $0
  nsExec::ExecToLog 'schtasks /Delete /TN "SmallTVUsage" /F'
  Pop $0

  SetOutPath "$TEMP"
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$INSTDIR\app"
  RMDir /r "$INSTDIR\app.smalltv-old"
  RMDir /r "$INSTDIR\app.smalltv-newfailed"

  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"

  Delete "$TEMP\smalltv-update.log"
  Delete "$TEMP\smalltv-update-app.ps1"
  Delete "$TEMP\smalltv-worker-check.ps1"
  RMDir /r "$TEMP\smalltv-emulator-update"

  IfSilent remove_appdata
  MessageBox MB_YESNO|MB_ICONEXCLAMATION "Also remove SmallTV settings, autosave, cached images and credentials? This includes Python daemon credentials in $APPDATA\smalltv." IDNO skip_appdata
  remove_appdata:
  RMDir /r "$APPDATA\smalltv"
  skip_appdata:

  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
