# The-Order-1886-Nativo-PC — TOport

Port nativo do **The Order: 1886** (PS4, CUSA00785) para Windows, construído em cima do
[BBPort](https://github.com/Supermedo/bloodborne_pc), o port nativo do Bloodborne. O nome do
projeto é **TOport**.

O código do jogo (x86-64, igual ao do PC) roda direto no processador, sem emulação de CPU. Só o que
muda entre PS4 e PC é traduzido: as funções do sistema da Sony (reescritas num runtime pequeno) e a GPU
(comandos GNM e shaders traduzidos para Vulkan pelo núcleo de vídeo do shadPS4).

**Nenhum arquivo do jogo está neste repositório.** É preciso um dump próprio, descriptografado, do
CUSA00785 (versão 1.02).

## Estado (pré-lançamento, 14/10/2026)
O jogo **roda do começo ao fim dos trechos testados, sem crash**, com cutscenes, combate e
exploração de Londres funcionando. Com um save de jogo completo, **todos os capítulos estão sendo
testados** agora pela seleção de capítulos, e cada nova área confirma a base.

### O que já funciona
- **Modo Original do PS4 (padrão):** a câmera cinematográfica 2.40:1 do jogo, com as faixas pretas
  nativas e a cena em 1920x800, exatamente como no console. Descobrimos que o executável do dump
  trazia embutidos os patches "720p" e "16:9" do illusion0001; o TOport restaura os valores
  originais na memória (sem tocar nos arquivos). Com isso **a iluminação global ficou muito mais
  estável** (o telhado, um dos pontos mais difíceis, ficou 100% correto). As opções 16:9 em tela
  cheia continuam disponíveis.
- **NVIDIA DLSS no lugar da resolução temporal do próprio jogo:** DLAA, Qualidade, Equilibrado,
  Desempenho e Ultra Desempenho, todos funcionando sobre a cena 2.40 (preset K).
- **Carregamento acelerado:** filas de compute e de leitura sem esperas desnecessárias. As esperas do
  fluxo de desenho caíram de ~4.600 para ~1.700 por segundo e as esperas por sinal de ~170 para ~53,
  **sem perder nenhuma textura ou objeto da cena**. Texturas e iluminação chegam bem mais rápido.
- **CPU:** trava de leitura/escrita (SRW) na tabela de memória, +25-30% de desenhos por segundo.
- **Rostos:** correções específicas para os pontos pretos nos rostos dos personagens já mapeados
  (Grayson, cena da cela, cutscenes); os demais estão sendo mapeados durante a varredura.
- **Pré-carga de shaders:** ao abrir, o jogo já monta os shaders usados nas duas últimas sessões.
- **Idiomas:** os 23 idiomas do sistema do PS4, com português do Brasil por padrão.
- **Câmera livre nas cutscenes** (modo foto) e limite de quadros 30/60/90/120 (F9).

### Desempenho (i5-10400F, RTX 2060 6 GB, 16 GB de RAM)
| Configuração | Cenas pesadas |
|---|---|
| Original + TAA do jogo | ~45–50 FPS |
| Original + DLSS Qualidade | ~40–45 FPS (modos mais baixos sobem mais) |
| 16:9 1080p + DLSS Qualidade (base fiel anterior) | ~44 FPS |

A primeira visita a cada área ainda compila shaders (travadinhas curtas); da segunda vez em diante o
cache e a pré-carga deixam tudo mais suave.

### Em andamento
- Varredura capítulo por capítulo com o save completo: piscadas de iluminação que restam em algumas
  áreas e rostos com pontos pretos ainda não mapeados.
- Uma cutscene com sincronia a revisar e um detalhe no início de uma cena.
- 1440p e 4K: testados e ainda não suportados (artefatos), fora das opções por enquanto.

### Requisitos recomendados
GPU Vulkan 1.3 com 6 GB de VRAM (DLSS: NVIDIA RTX), 16 GB de RAM (feche navegador e apps pesados ao
jogar; o port fecha sozinho se a RAM livre cair abaixo de 700 MB), CPU de 6 núcleos.

## Jogar (TOport)
1. Abra `TOport.exe` (pacote gerado por `packaging/windows/package_toport.sh`).
2. Em "Jogo e idioma", aponte a pasta do seu dump (CUSA00785 1.02).
3. Ajuste resolução (Original do PS4 recomendado), DLSS e idioma, e clique em **JOGAR**.

No jogo: **Select** (controle) ou **Insert** (teclado) abre o menu do TOport; **F9** alterna o
limite de quadros. Teclado: Espaço = ✕, Shift = ◯, E = □, Q = △, WASD = analógico esquerdo,
setas = analógico direito, Enter = Options.

## Compilar (Windows)
1. MSYS2 com o ambiente CLANG64 e os pacotes listados em `packaging/windows/README.md`.
2. `bash build.sh` (com `MSYS2_ROOT` apontando para o MSYS2, se não estiver em `C:\msys64`).
3. `bash packaging/windows/package_toport.sh` gera `dist/TOport/` e `dist/TOport.zip`.
   Para desenvolvimento: `BB_GAME_DIR=<pasta do CUSA00785> python run.py`.

## Agradecimentos
O TOport só existe porque outras pessoas abriram o caminho e publicaram o seu trabalho:
- **[BBPort](https://github.com/Supermedo/bloodborne_pc)**, de deadinside28 (Linux) e Supermedo
  (Windows): a base inteira do port, do carregador nativo ao launcher e ao menu do jogo. O README
  original está em [`README.bbport.md`](README.bbport.md).
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** e a sua equipe: núcleo de vídeo Vulkan e
  recompilador de shaders. As anotações sobre The Order no
  [PR #4798](https://github.com/shadps4-emu/shadPS4/pull/4798) ajudaram na investigação das sombras.
- **Ready at Dawn**, pela palestra do SIGGRAPH 2015 sobre o renderizador do jogo, que guiou a
  integração do DLSS no lugar da resolução temporal original.
- **NVIDIA**, pelo DLSS (SDK distribuído sob a licença em `licenses/`).
- Projetos consultados durante o desenvolvimento, com ideias testadas aqui:
  [ShadowOfTheColossus-PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC) (AjncSystem), os
  ports do HayatoG (inFAMOUS: Second Son e outros) e o [bbhost](https://github.com/droogie/bbhost)
  (droogie).
- O auxiliador (Codex), pelas correções de rosto, partículas, mira e pelo menu em português.

## Aviso legal
- Projeto de fã, sem fins lucrativos, **sem qualquer ligação, aprovação ou patrocínio** da Sony
  Interactive Entertainment, da Ready at Dawn Studios ou da NVIDIA.
- "PlayStation", "PS4" e os respectivos logotipos são marcas registradas da Sony Interactive
  Entertainment Inc. "The Order: 1886" é marca da Sony Interactive Entertainment LLC. Todas as
  outras marcas pertencem aos seus donos.
- **Nenhum arquivo do jogo é distribuído**: nem executável, nem dados, nem shaders, nem imagens.
  Para jogar é preciso o seu próprio dump de uma cópia que você possui legalmente (CUSA00785,
  versão 01.02). O cache de shaders é gerado na máquina de cada jogador.
- Este repositório não contém nem ensina a contornar proteções de cópia. Não peça nem compartilhe
  arquivos do jogo nas issues.
- O software é fornecido "como está", sem garantias. Use por sua conta e risco.

## Licença
GPL-2.0-or-later, herdada do BBPort e do shadPS4. Bibliotecas de terceiros: ver
`README.bbport.md`. O DLSS segue a licença da NVIDIA em `licenses/NVIDIA-DLSS-LICENSE.txt`.
