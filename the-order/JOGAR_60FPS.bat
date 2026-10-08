@echo off
rem The Order: 1886 com limite de 60 FPS (o padrao agora e 30, como no PS4).
rem A 60 as cutscenes podem quebrar. F9 durante o jogo alterna entre 30 e 60.
set BB_FPS_LIMIT=60
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" 0
pause
