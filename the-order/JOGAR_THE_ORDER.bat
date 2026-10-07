@echo off
rem Compila (se precisar) e abre o port do The Order: 1886.
rem Uso: duplo clique (10 minutos) ou "JOGAR_THE_ORDER.bat 1800" para outro limite em segundos.
set TEMPO=%1
if "%TEMPO%"=="" set TEMPO=600
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" %TEMPO%
pause
