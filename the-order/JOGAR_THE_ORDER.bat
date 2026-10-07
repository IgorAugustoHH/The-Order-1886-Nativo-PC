@echo off
rem Compila (se precisar) e abre o port do The Order: 1886.
rem Sem limite de tempo: o jogo fecha quando voce fechar a janela dele.
rem Para testes com limite: "JOGAR_THE_ORDER.bat 600" (segundos).
set TEMPO=%1
if "%TEMPO%"=="" set TEMPO=0
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" %TEMPO%
pause
