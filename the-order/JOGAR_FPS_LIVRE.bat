@echo off
rem TESTE: ignora a trava de 30 FPS que o jogo pede (BB_FLIP_RATE=0).
rem Repare se animacoes, cutscenes ou fisica ficam rapidas demais.
set BB_FLIP_RATE=0
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" 0
pause
