@echo off

echo compiling shader...
if "%1" == "" GOTO INCORRECT_N_ARGS
if "%2" == "" GOTO SET_DEBUG
if "%2" == "debug"   GOTO SET_DEBUG
if "%2" == "release" GOTO SET_RELEASE

:SET_DEBUG
echo Debug Build...
set dbg_flags = "/Od /Zi"
GOTO BUILD

:SET_RELEASE
echo Release Build...
set dbg_flags = " "
GOTO BUILD

:BUILD
echo Shader Filename: '%1'

set vs_targ=vs_5_0
set vs_entry="VS"

set ps_targ=ps_5_0
set ps_entry="PS"

set defines=/D BATCH_AMOUNT=1024
set prefix=%~d1%~p1%~n1

rem echo fxc %1 %dbg_flags% /T vs_5_0 /E “VS” /Fo “color_vs.cso” /Fc “color_vs.asm”

rem echo Drive:  %~d1
rem echo Path:   %~p1
rem echo Name:   %~n1
rem echo Ext:    %~x1
rem echo Prefix: %prefix%

rem don't care about cso, just asm
rem fxc %1 %dbg_flags% /T %vs_targ% /E %vs_entry% %defines% /Fo "%prefix%_vs.cso" /Fc "%prefix%_vs.asm"
rem fxc %1 %dbg_flags% /T %ps_targ% /E %ps_entry% %defines% /Fo "%prefix%_ps.cso" /Fc "%prefix%_ps.asm"
fxc %1 %dbg_flags% /T %vs_targ% /E %vs_entry% %defines% /Fc "%prefix%_vs.asm"
fxc %1 %dbg_flags% /T %ps_targ% /E %ps_entry% %defines% /Fc "%prefix%_ps.asm"

exit /B

:INCORRECT_N_ARGS
echo Incorrect number of arguments!
echo Usage: comp_shaders filename.hlsl