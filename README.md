# The-Order-1886-Nativo-PC

Port nativo e experimental do **The Order: 1886** (PS4, CUSA00785) para Windows, construído em cima do
[BBPort](https://github.com/Supermedo/bloodborne_pc), o port nativo do Bloodborne.

O código do jogo (x86-64, igual ao do PC) roda direto no processador, sem emulação de CPU. Só o que
muda entre PS4 e PC é traduzido: as funções do sistema da Sony (reescritas num runtime pequeno) e a GPU
(comandos GNM e shaders traduzidos para Vulkan pelo núcleo de vídeo do shadPS4).

**Nenhum arquivo do jogo está neste repositório.** É preciso um dump próprio, descriptografado, do
CUSA00785 (testado na versão 1.02).

## Estado
- Abre a abertura, a tela de título e o menu principal; responde ao controle e ao teclado.
- NEW GAME: o primeiro save automático é criado (`Playthrough00`); em teste a partir daí.
- Pendências: shaders com tabelas de recursos (SRT) que o recompilador não resolve (texturas
  faltando/erradas), MSAA desligado, travamento intermitente no driver da NVIDIA ao iniciar.

Notas detalhadas, histórico das correções e o que falta: [`the-order/PROJETO.md`](the-order/PROJETO.md).

## Compilar e rodar (Windows)
1. MSYS2 com o ambiente CLANG64 e os pacotes listados em `packaging/windows/README.md`.
2. `bash build.sh` (com `MSYS2_ROOT` apontando para o MSYS2, se não estiver em `C:\msys64`).
3. `BB_GAME_DIR=<pasta do CUSA00785> python run.py`

Para desenvolvimento, `the-order/iterar.sh` faz o ciclo completo: gera os nomes das funções,
compila, roda o jogo, fecha e mostra onde parou. `the-order/JOGAR_THE_ORDER.bat` faz o mesmo por
duplo clique.

Teclado: Espaço = ✕, Shift = ◯, E = □, Q = △, WASD = analógico esquerdo, setas = analógico direito,
Enter = Options.

## Créditos e licença
GPL-2.0-or-later, herdada do BBPort e do shadPS4.
- [BBPort](https://github.com/Supermedo/bloodborne_pc): deadinside28 (Linux), Supermedo (Windows).
  O README original está em [`README.bbport.md`](README.bbport.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4): núcleo de vídeo e recompilador de shaders.
- Demais bibliotecas: ver `README.bbport.md`.
