#!/usr/bin/env bash
# Um ciclo de desenvolvimento: nomes NID -> compila -> roda o jogo -> mostra onde parou.
# Uso (Git Bash): bash iterar.sh [segundos]   (padrão: 90 s; 0 = sem limite)
set -u
RAIZ="/b/Projeto the order 1886"
BB="$RAIZ/bbport-base"
N=$(ls "$RAIZ"/work/run*.log 2>/dev/null | wc -l)
LOG="$RAIZ/work/run$((N + 1)).log"
# Esperas pela GPU (Scheduler::Wait/Finish) de 0,5 ms ou mais, com a origem no código
# (bb_wait_trace.h, do auxiliador). Resumo: python analise/analisar_esperas.py work/esperas_runN.txt
export BB_WAIT_TRACE="${BB_WAIT_TRACE:-1}" BB_WAIT_TRACE_FILE="B:/Projeto the order 1886/work/esperas_run$((N + 1)).txt"

python "$BB/tools/gen_import_names.py" >/dev/null || exit 1
if ! MSYSTEM=CLANG64 /b/msys64/usr/bin/bash.exe -lc "cd \"$BB\" && MSYS2_ROOT=/b/msys64 bash build.sh" >"$RAIZ/work/build.log" 2>&1; then
    grep -iE "error|undefined" "$RAIZ/work/build.log" | head -20
    echo "BUILD FALHOU (work/build.log)"; exit 1
fi
# Camadas Vulkan implícitas (OBS, RivaTuner, TikTok LIVE, Steam, Epic) se injetam em todo app Vulkan;
# com elas o driver da NVIDIA caía no início (nvoglv64.dll). Desligadas só para o jogo.
# BB_COM_CAMADAS=1 bash iterar.sh  -> mantém as camadas (para comparar).
[ -z "${BB_COM_CAMADAS:-}" ] && export VK_LOADER_LAYERS_DISABLE='~implicit~'
# Estatísticas de quadro (FPS, pior quadro, compilação de shaders, travadas) a cada 5 s no log.
# BB_SEM_STATS=1 bash iterar.sh -> desliga.
[ -z "${BB_SEM_STATS:-}" ] && export BB_FRAME_STATS=1 BB_PERM_TRACE=1
# Tela cheia sem bordas (SDL3): em janela, a área 1920x1080 + barra de título e bordas passava do
# tamanho do monitor e cortava os lados. BB_JANELA=1 bash iterar.sh -> abre em janela.
[ -z "${BB_JANELA:-}" ] && export BB_FULLSCREEN=1
# Upscaler, jitter e vetores de movimento do BBPort foram feitos para o Bloodborne (o FSR procura um
# shader dele): no The Order só tremem a imagem e gastam CPU/GPU. Desligados até adaptarmos.
# BB_COM_UPSCALER=1 bash iterar.sh -> usa a configuração do menu (work/bbport.ini).
[ -z "${BB_COM_UPSCALER:-}" ] && export BB_UPSCALER=none BB_JITTER=0 BB_OBJECT_MOTION=0
# FPS: com vblank de 60 Hz o flip só sai no próximo tique, e o jogo espera o "flip concluído" antes
# do quadro seguinte: um quadro de 18 ms vira 33 ms (trava em 30). BB_VBLANK_HZ=0 mostra o quadro
# assim que fica pronto (o jogo não usa o vblank como relógio, só o evento de flip).
# Limite padrão 60 (decisão do Igor, 08/10). O jogo foi feito e testado só a 30 (escolha "cinematográfica" da
# Ready at Dawn) e a 60 as cutscenes quebram; a 30 ficou muito mais estável (run51).
# F9 no jogo alterna 30/60. BB_FPS_LIMIT=60 bash iterar.sh -> começa em 60; 0 -> sem limite.
export BB_VBLANK_HZ="${BB_VBLANK_HZ:-0}" BB_FPS_LIMIT="${BB_FPS_LIMIT:-60}"
# Cache de pipelines ligado de novo (08/10). Ele guardava as variantes pelo número de chegada
# na sessão: o mesmo número era outra variante em outra sessão, e os pipelines pré-carregados
# saíam com o shader errado (menu com partes faltando nas run48-51, tela preta nas run53-54).
# Agora as variantes são guardadas pelo conteúdo. O banco antigo ficou em user/cache_antigo4.
# BB_PIPELINE_CACHE=0 bash iterar.sh -> desliga.
export BB_PIPELINE_CACHE="${BB_PIPELINE_CACHE:-1}"
# Chaves de otimização ao vivo: o número em work/toggles.txt (lido a cada 0,25 s) desliga as
# otimizações do BBPort cujos bits estiverem ligados (gpu/shim/bbport_toggles.h). 0 = todas ligadas.
export BB_TOGGLE_FILE="${BB_TOGGLE_FILE:-B:/Projeto the order 1886/work/toggles.txt}"
# Padrão do The Order: DeferredStreamCopies (16384) desligada. A cópia adiada das constantes
# pequenas (até 16 KB) acontecia depois que o processador de comandos já tinha aplicado as
# escritas de memória dos comandos seguintes: objetos, paredes e tetos piscavam com as
# constantes de outro desenho (achado na run60, desligando as otimizações uma a uma).
# BB_TOGGLES=0 bash iterar.sh -> todas ligadas.
printf '%s' "${BB_TOGGLES:-16384}" > "B:/Projeto the order 1886/work/toggles.txt"
# Ferramentas de shader ao vivo (vk_rasterizer.cpp): work/live/skip_shaders.txt pula os shaders
# listados (hashes hex); criar work/live/capture_shaders.req grava em captured_shaders.txt os
# shaders usados em 300 ms.
mkdir -p "/b/Projeto the order 1886/work/live"; : > "/b/Projeto the order 1886/work/live/skip_shaders.txt"
export BB_LIVE_DIR="${BB_LIVE_DIR:-B:/Projeto the order 1886/work/live}"
# Análise de quadro (vk_frame_capture.cpp): criar work/live/capturar_quadro.req grava o próximo
# quadro inteiro (cada passada: alvos, formatos, shaders, texturas; cada compute) em
# work/capturas/frame_*.txt. Nomes dos shaders: analise/shaders_nomes.txt.
mkdir -p "/b/Projeto the order 1886/work/capturas"; rm -f "/b/Projeto the order 1886/work/live/capturar_quadro.req"
export BB_CAPTURE_TRIGGER="${BB_CAPTURE_TRIGGER:-B:/Projeto the order 1886/work/live/capturar_quadro.req}"
export BB_CAPTURE_DIR="${BB_CAPTURE_DIR:-B:/Projeto the order 1886/work/capturas}"
# Estudo do laço do SRT (temporário): grava o IR destes shaders em work/user/shader/dumps.
# Material que já aparece no menu, vértice, compute e outro vértice com "loop index".
# depthreduction (clear_xy, msaa2x_lag_xy, finalpass): as cascatas do sol saem minúsculas.
# BB_RETRANSLATE ignora o cache deles para que sejam traduzidos (e gravados) de novo.
export BB_DUMP_ONLY="${BB_DUMP_ONLY:-7550d841,c6941be0,7b1b18c9}"
export BB_RETRANSLATE="${BB_RETRANSLATE:-$BB_DUMP_ONLY}"
# Modo validação (bem mais lento): as camadas de validação da Khronos conferem cada chamada Vulkan e
# gravam os erros em work/validacao.txt. BB_VALIDACAO=1 bash iterar.sh
if [ -n "${BB_VALIDACAO:-}" ]; then
    export VK_ADD_LAYER_PATH='B:\msys64\clang64\bin' VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    export VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG
    export VK_KHRONOS_VALIDATION_REPORT_FLAGS=error
    export VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=3
    export VK_KHRONOS_VALIDATION_LOG_FILENAME="B:/Projeto the order 1886/work/validacao.txt"
    rm -f "$RAIZ/work/validacao.txt"
fi
cd "$BB" && BB_GAME_DIR="B:/Projeto the order 1886/CUSA00785" BB_DATA_DIR="B:/Projeto the order 1886/work" \
    MSYS2_ROOT="B:/msys64" timeout "${1:-90}" python run.py >"$LOG" 2>&1
R=$?
# o timeout encerra o run.py; o jogo (bb-probe) pode sobrar segurando RAM/VRAM
powershell -NoProfile -Command "Get-Process bb-probe -ErrorAction SilentlyContinue | Stop-Process -Confirm:\$false" >/dev/null 2>&1
echo "EXIT $R -> $LOG"
echo "iterar.sh: run.py terminou com $R" >> "$LOG"
grep -E "^STOP|^API|^Caller|first argument|exception|fault|Unhandled|GPU:|rror" "$LOG" | head -20
grep -vE "^Runtime: (guest thread|semaphore|mapped|direct allocation)" "$LOG" | grep -B12 -m1 "^STOP" | head -14
