@echo off
rem Abre o port com as camadas de validacao do Vulkan (bem mais lento).
rem Os erros encontrados ficam em work\validacao.txt
set BB_VALIDACAO=1
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" 0
pause
