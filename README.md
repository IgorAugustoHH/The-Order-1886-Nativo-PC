# The-Order-1886-Nativo-PC

Port nativo e experimental do **The Order: 1886** (PS4, CUSA00785) para Windows, construído em cima do
[BBPort](https://github.com/Supermedo/bloodborne_pc), o port nativo do Bloodborne.

O código do jogo (x86-64, igual ao do PC) roda direto no processador, sem emulação de CPU. Só o que
muda entre PS4 e PC é traduzido: as funções do sistema da Sony (reescritas num runtime pequeno) e a GPU
(comandos GNM e shaders traduzidos para Vulkan pelo núcleo de vídeo do shadPS4).

**Nenhum arquivo do jogo está neste repositório.** É preciso um dump próprio, descriptografado, do
CUSA00785 (testado na versão 1.02).

## Estado (pré-lançamento, 10/10/2026)
O jogo é **jogável nos trechos testados**, sem crash: abertura, cela, masmorra, becos e
pátios de Londres, cutscenes e combate. Iluminação do sol e sombras em cascata funcionam como no PS4.
Versão de referência: tag `versao-60fps-2026-10-09`, launcher `the-order/JOGAR_60FPS.bat`.

### Desempenho medido (maior sessão: run114, ~30 min, várias áreas, cache de shaders vazio)
PC de teste: Intel i5-10400F, RTX 2060 6 GB, 16 GB de RAM, 1080p, limite de 120 FPS.

| Métrica | Valor |
|---|---|
| FPS médio / mediano | 72,8 / 67,4 |
| 10% piores janelas de 5 s | 43 FPS ou menos (mínimo 30) |
| Pior quadro | 710 ms (sempre durante compilação de shaders) |
| Compilações de shader na sessão | 10.481 |
| VRAM (pico) | 4,1 de 5,2 GB disponíveis |
| RAM livre mínima | 2,6 GB (o jogo usa ~5 GB) |
| Crashes | nenhum |

Segunda sessão, com o cache montado (run115, ~11 min): FPS mediano 90, 10% piores em 53.
A primeira visita a cada área tem travadinhas e luz "se ajeitando" enquanto os shaders compilam.

### Qualidade visual
- Correto: luz do sol, sombras, texturas, materiais, nevoeiro, cutscenes, interface.
- Problemas conhecidos (em tratamento):
  - pontos pretos hexagonais no rosto de personagens masculinos (suspeita: bokeh do desfoque de profundidade);
  - personagens escuros em parte da cutscene da cela;
  - triângulos de geometria "explodida" em alguns objetos e sombras (masmorra);
  - partículas e folhas brancas;
  - fumaça da granada com defeito;
  - granulado em fachadas distantes (pode ser o grão de filme do próprio jogo).
- Desempenho em tratamento: só 2 threads compilam shaders; um shader de material gera centenas de variações.

Lista técnica completa: [`the-order/PASSAGEM_PARA_AUXILIADOR.md`](the-order/PASSAGEM_PARA_AUXILIADOR.md).
Histórico: [`the-order/PROJETO.md`](the-order/PROJETO.md).

### Requisitos recomendados
GPU Vulkan 1.3 com 6 GB de VRAM, 16 GB de RAM (feche navegador e apps pesados ao jogar; o port fecha
sozinho se a RAM livre cair abaixo de 700 MB), CPU de 6 núcleos.

## Compilar e rodar (Windows)
1. MSYS2 com o ambiente CLANG64 e os pacotes listados em `packaging/windows/README.md`.
2. `bash build.sh` (com `MSYS2_ROOT` apontando para o MSYS2, se não estiver em `C:\msys64`).
3. `BB_GAME_DIR=<pasta do CUSA00785> python run.py`

Para desenvolvimento, `the-order/iterar.sh` faz o ciclo completo: gera os nomes das funções,
compila, roda o jogo, fecha e mostra onde parou. `the-order/JOGAR_60FPS.bat` (recomendado) e
`the-order/JOGAR_THE_ORDER.bat` fazem o mesmo por duplo clique. F9 alterna 30/60/90/120 FPS.

Teclado: Espaço = ✕, Shift = ◯, E = □, Q = △, WASD = analógico esquerdo, setas = analógico direito,
Enter = Options.

## Créditos e licença
GPL-2.0-or-later, herdada do BBPort e do shadPS4.
- [BBPort](https://github.com/Supermedo/bloodborne_pc): deadinside28 (Linux), Supermedo (Windows).
  O README original está em [`README.bbport.md`](README.bbport.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4): núcleo de vídeo e recompilador de shaders.
- Demais bibliotecas: ver `README.bbport.md`.
