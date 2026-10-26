; GPU Fusion installer (NSIS 3, Modern UI 2). Built by scripts\package.ps1:
;   makensis /DVERSION=1.0.0 /DSRCDIR=<portable folder> /DOUTFILE=<setup.exe> /DICON=<ico> GPUFusion.nsi
;
; Per-user install, no administrator rights: program files go to %LOCALAPPDATA%\Programs\GPU Fusion,
; settings/logs/benchmarks to its "data" subfolder (created by the app on first start).
; Writes only: the program folder, a Start-menu shortcut, an optional desktop shortcut and the
; per-user "Apps & features" uninstall entry (HKCU). No services, drivers or system settings.
;
; /TESTINSTALL (with /S /D=<dir>) installs files only — no shortcuts, no uninstall entry — so the
; installer can be verified on a development machine without changing it.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !error "VERSION not defined"
!endif

Name "GPU Fusion ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\GPU Fusion"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "GPU Fusion ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "GPU Fusion"
VIAddVersionKey "FileDescription" "GPU Fusion Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 GPU Fusion contributors"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\GPUFusion.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start GPU Fusion"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\GPUFusion"
Var TestInstall

Function .onInit
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/TESTINSTALL" $1
  ${IfNot} ${Errors}
    StrCpy $TestInstall 1
  ${EndIf}
FunctionEnd

Section "GPU Fusion (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall GPU Fusion.exe"
  ${If} $TestInstall != 1
    CreateShortCut "$SMPROGRAMS\GPU Fusion.lnk" "$INSTDIR\GPUFusion.exe" "" "$INSTDIR\GPUFusion.exe" 0
    WriteINIStr "$INSTDIR\install.ini" "Shortcuts" "StartMenu" "1"
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "GPU Fusion"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "GPU Fusion contributors"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\GPUFusion.exe"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall GPU Fusion.exe"'
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${EndIf}
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  ${If} $TestInstall != 1
    CreateShortCut "$DESKTOP\GPU Fusion.lnk" "$INSTDIR\GPUFusion.exe" "" "$INSTDIR\GPUFusion.exe" 0
    WriteINIStr "$INSTDIR\install.ini" "Shortcuts" "Desktop" "1"
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The application, its Qt and Visual C++ runtime files, and the GPU preference probe."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Adds a GPU Fusion shortcut to your desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; Program files only. Settings, logs and benchmark results in "data" are kept unless the user agrees.
  ${If} ${FileExists} "$INSTDIR\data\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete GPU Fusion's settings, logs and benchmark results ($INSTDIR\data)?" /SD IDNO IDNO keepData
    RMDir /r "$INSTDIR\data"
  keepData:
  ${EndIf}
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\styles"
  RMDir /r "$INSTDIR\imageformats"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\sdk"
  RMDir /r "$INSTDIR\docs"
  ; Only shortcuts this installer created (recorded in install.ini) — never one the user made.
  ReadINIStr $0 "$INSTDIR\install.ini" "Shortcuts" "StartMenu"
  ${If} $0 == "1"
    Delete "$SMPROGRAMS\GPU Fusion.lnk"
    DeleteRegKey HKCU "${UNINST_KEY}"
  ${EndIf}
  ReadINIStr $0 "$INSTDIR\install.ini" "Shortcuts" "Desktop"
  ${If} $0 == "1"
    Delete "$DESKTOP\GPU Fusion.lnk"
  ${EndIf}
  Delete "$INSTDIR\install.ini"
  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.txt"
  Delete "$INSTDIR\*.md"
  RMDir "$INSTDIR"
SectionEnd
