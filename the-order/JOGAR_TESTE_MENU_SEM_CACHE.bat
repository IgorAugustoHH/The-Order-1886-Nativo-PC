@echo off
rem TESTE do menu: abre sem o cache de pipelines salvo em disco (compila tudo de novo,
rem entao tem mais travadas). Serve para ver se o bug visual do menu vem do cache.
set BB_PIPELINE_CACHE=0
"C:\Program Files\Git\bin\bash.exe" "/b/Projeto the order 1886/iterar.sh" 0
pause
