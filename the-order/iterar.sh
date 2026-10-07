#!/usr/bin/env bash
# Um ciclo de desenvolvimento: nomes NID -> compila -> roda o jogo -> mostra onde parou.
# Uso (Git Bash): bash iterar.sh [segundos]   (padrão: 90 s; 0 = sem limite)
set -u
RAIZ="/b/Projeto the order 1886"
BB="$RAIZ/bbport-base"
N=$(ls "$RAIZ"/work/run*.log 2>/dev/null | wc -l)
LOG="$RAIZ/work/run$((N + 1)).log"

python "$BB/tools/gen_import_names.py" >/dev/null || exit 1
if ! MSYSTEM=CLANG64 /b/msys64/usr/bin/bash.exe -lc "cd \"$BB\" && MSYS2_ROOT=/b/msys64 bash build.sh" >"$RAIZ/work/build.log" 2>&1; then
    grep -iE "error|undefined" "$RAIZ/work/build.log" | head -20
    echo "BUILD FALHOU (work/build.log)"; exit 1
fi
# Camadas Vulkan implícitas (OBS, RivaTuner, TikTok LIVE, Steam, Epic) se injetam em todo app Vulkan;
# com elas o driver da NVIDIA caía no início (nvoglv64.dll). Desligadas só para o jogo.
# BB_COM_CAMADAS=1 bash iterar.sh  -> mantém as camadas (para comparar).
[ -z "${BB_COM_CAMADAS:-}" ] && export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$BB" && BB_GAME_DIR="B:/Projeto the order 1886/CUSA00785" BB_DATA_DIR="B:/Projeto the order 1886/work" \
    MSYS2_ROOT="B:/msys64" timeout "${1:-90}" python run.py >"$LOG" 2>&1
R=$?
# o timeout encerra o run.py; o jogo (bb-probe) pode sobrar segurando RAM/VRAM
powershell -NoProfile -Command "Get-Process bb-probe -ErrorAction SilentlyContinue | Stop-Process -Confirm:\$false" >/dev/null 2>&1
echo "EXIT $R -> $LOG"
grep -E "^STOP|^API|^Caller|first argument|exception|fault|Unhandled|GPU:|rror" "$LOG" | head -20
grep -vE "^Runtime: (guest thread|semaphore|mapped|direct allocation)" "$LOG" | grep -B12 -m1 "^STOP" | head -14
