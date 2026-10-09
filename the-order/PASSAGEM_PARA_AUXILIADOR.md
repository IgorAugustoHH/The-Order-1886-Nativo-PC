# Passagem para o auxiliador (Codex) — 09/10/2026

Escrito por Claude a pedido do Igor. Igor vai passar a carga principal para o auxiliador
ate domingo; depois disso pretende lancar a versao 1.0 no GitHub. Este documento lista o
que ficou para fazer, o que ja foi medido e onde mexer.

## 1. Versao de referencia ("a versao de 60 FPS")

- Repositorio principal: `bbport-base`, branch `the-order`, tag local `versao-60fps-2026-10-09`
  (commit do mesmo dia; ver `git log`). Nada foi enviado ao GitHub.
- Launcher de referencia: `JOGAR_60FPS.bat` na raiz do projeto (`BB_FPS_LIMIT=60`, F9 alterna
  30/60/90/120). E a versao que o Igor validou: sem crash em ~40 min de jogo.
- Commits desta rodada, em cima do que o auxiliador ja conhece:
  - `61d3565` integra a correcao ADDR64 do auxiliador (`ReadGuestU32`, parser de faltas com
    1023 slots, `BB_DMA_CLEAN_SKIP`, `BB_DUMP_ONLY`). Agora **ligado por padrao**:
    `BB_DIRECT_MEMORY_ACCESS`, `BB_ADDR64_LOADS`, `BB_DMA_CLEAN_SKIP` (=0 desliga cada um).
    `ShaderBinaryVersion` = 8. Os rastreadores de descritores do auxiliador NAO entraram.
  - `7aad56e` contador global `cpu_modified_generation` (region_manager.h): incrementado
    sempre que uma pagina vira CPU-modified (falta de escrita, ExtendWriteFault, hot pages,
    regiao nova) ou um intervalo vira residente. `SynchronizeDmaBuffers` pula a varredura
    inteira se o contador nao mudou. `BB_DMA_SYNC_GENERATION=0` desliga.
    Medido no mesmo beco, janelas de 5 s: 1,33-1,37 s de CPU (varredura completa,
    `BB_DMA_CLEAN_SKIP=0`) contra 0,24-0,31 s; 95% das chamadas puladas.
  - `iterar.sh` liga `BB_DMA_TRACE=1` (linhas "DMA sync"/"DMA faults" a cada 5 s).
- Padroes ainda ativos: pipelines assincronos (draw/dispatch sem pipeline pronto e
  **pulado**), cache de pipelines por conteudo, pre-carga dos pipelines das 2 ultimas
  sessoes (`work/user/shader/pipelines_recentes.txt`), readbacks Relaxed adiados.

## 2. Medicoes que servem de linha de base

| Log | Situacao | FPS (janelas 5 s) | Outros |
|---|---|---|---|
| run114 | cache vazio, ~30 min, varias areas | media 72,8; mediana 67,4; p10 43; min 30 | piores quadros 250-710 ms, todos em janelas com compilacao; 10.481 compilacoes (812 s somados); RAM livre minima 2,6 GB; VRAM pico 4,1/5,2 GB; 0 paginas DMA faltantes |
| run115 | cache quente, ~11 min, cela inicial e masmorra | mediana 90; media 86; p10 53 | 5.848 pipelines pre-carregados; 2.647 compilacoes; mesmos defeitos visuais |
| run111 | primeira build integrada, sem 7aad56e | 26-41 no beco | thread de gravacao (stage B) 86-88% ocupada; fechou pela protecao de RAM (so 5,7 GB livres ao abrir, outros apps abertos) |

Conclusao medida: os defeitos visuais abaixo **continuam com o cache quente**, entao nao
sao causados pelos draws pulados durante a compilacao (Claude chegou a supor isso; o
run115 descartou).

## 3. Defeitos visuais (prioridade do Igor)

Prints do Igor em 09/10 (cutscene da cela, masmorra/forja, sala com manequim, beco).

1. **Pontos pretos hexagonais no rosto dos personagens masculinos** (cutscenes e gameplay,
   em quase toda cutscene). Ficam onde a pele brilha (testa, macas suadas). Hipotese NAO
   confirmada: o bokeh do DOF (sprites hexagonais) e gerado nesses brilhos, mas sai com cor
   preta. Shaders (analise/shaders_nomes.txt): `bokeh_ps` 0xea239e56, `bokeh_vs` 0x0498060d,
   `bokeh_gs` hs/ls 0xbcdf323e70f775aa, `bokeh_gs_vs` 0xbcdf323ee3bcba89,
   `adjustbokehargs_cs` 0xd46bc001.
   Teste barato ao vivo: com o jogo na cena, escrever os hashes em
   `work/live/skip_shaders.txt` (lido a cada 250 ms; ver comentario no topo de
   `vk_rasterizer.cpp`; confirmar o formato de hash esperado para os de 64 bits). Se os
   pontos sumirem, rastrear de onde vem a cor do sprite (caminho ES/GS emulado ou o buffer
   que o adjustbokehargs prepara). Se nao sumirem, descartar a hipotese.
2. **Personagens pretos na cutscene da cela** (Grayson e o outro preso aparecem sem luz em
   parte dos planos). Persiste com cache. Suspeito principal: no run114 (cache vazio) 471
   fragment shaders registraram `SRT walker: 14 unresolved resource lookups ... (phi merges
   different offsets (loop index))`. Leituras de dados nao resolvidas ja vao para DMA
   (`flatten_extended_userdata_pass.cpp`, funcao com `FeedsDescriptor`, ~L934), mas as que
   alimentam **descritores** (T#/S# escolhidos dentro do laco) continuam sem resolucao.
   Metodo: captura de quadro na cena (`work/live/capturar_quadro.req`, saida em
   `work/capturas`, resumo com `analise/resumir_captura.py`), achar os draws do
   personagem e conferir os descritores ligados contra os bytes guest.
3. **Vertices explodidos**: triangulos/cunhas no gancho e cunhas pretas nas sombras do chao
   (masmorra), rosto do Grayson deformado uma vez no run111. Historicamente o FaceGen
   explodia sem readbacks; Relaxed corrigiu. Pontos a verificar:
   - O log mostra "bytes the CPU changed before the GPU copy arrived" > 0 nos readbacks
     adiados (32efe96). Conferir em `BufferCache::SynchronizeMemory` (ForEachNotInRange
     sobre pending_readbacks) que nenhuma escrita da CPU e perdida: os bits CPU sao
     limpos tambem para os bytes pulados.
   - A/B so por variavel: `BB_READBACKS`, `BB_DMA_CLEAN_SKIP=0`, `BB_DMA_SYNC_GENERATION=0`,
     `BB_ADDR64_LOADS=0` (este exige `BB_PIPELINE_CACHE=0`: o cache foi traduzido com ADDR64).
     Launchers prontos na raiz: `TESTE_A_SEM_PULO_DMA.bat`, `TESTE_B_SEM_ADDR64.bat`,
     `TESTE_C_SEM_DMA.bat`. O A ja rodou (run112): imagem pior por falta de CPU (texturas
     atrasadas), nao serve como build de jogo.
4. **Particulas brancas** (folhas, mancha branca perto da grade no beco):
   `pointspritelighting_cs` 0x804aeead e `renderbillboardsprites(_flowmap)_vs`
   0x8c8a4817 / 0xa8792bbb, todos com lookups nao resolvidos por indice de laco (run115).
   Mesma familia do item 2 e dos T# de 804aeead que o auxiliador ja investigava.
5. **Granada de fumaca**: antes nao aparecia; agora aparece com defeito. Nao investigado.
6. **Granulado/"chuvisco" em fachadas e ceu**: parte pode ser o grao de filme do proprio
   jogo. Comparar com video do PS4 antes de tratar como bug.

## 4. Desempenho de CPU: shaders e texturas (o que o Igor quer melhorar)

Objetivo: primeira visita a uma area sem os erros visuais e travadas de agora, e menos CPU
por quadro. Igor nao aceita perder fidelidade para ganhar FPS.

1. **So 2 threads de compilacao**: `constexpr u32 NumCompilers = 2;` em
   `PipelineCache::StartAsyncCompilers` (vk_pipeline_cache.cpp ~L592). O PC do Igor e um
   i5-10400F (6 nucleos/12 threads). Proposta: escalar pelo numero de nucleos, threads com
   prioridade baixa, `BB_COMPILE_THREADS` para sobrescrever. Medir RAM por thread do driver
   (16 GB no total, protecao de RAM em 700 MiB livres).
2. **Explosao de permutacoes**: no run114 o shader de material 0x539eee77 teve 149
   permutacoes (avisos `GetProgram: Permutation N of 0x539eee77: img20(type bindings),
   img25(type cube bindings), img30(srgb bindings)...`). Cada combinacao de tipo de
   imagem/sRGB gera um shader novo. Investigar se da para normalizar esses bindings sem
   mudar o resultado (menos compilacoes, menos cache).
3. **Draws pulados durante a compilacao**: causam luz faltando/"recalculando" na primeira
   visita. Opcoes: prioridade na fila para os passes de iluminacao, ou esperar com limite de
   tempo apenas para eles. Medir travada contra erro visual.
4. **Cache com entradas velhas**: o run115 relata `WarmUp: 10592 stale pipelines` (entradas
   da versao 7 que continuam no banco). Limpar no carregamento as chaves cujo shader e de
   versao antiga.
5. **Lista de pipelines recentes**: guarda as 2 ultimas sessoes; uma sessao curta empurra a
   lista boa para fora. Pensar em ponderar por tempo de jogo.
6. **DMA sync ainda custa ~5%** de uma thread: cada escrita nova da CPU dispara uma varredura
   de ~700 intervalos residentes. Uma lista de regioes sujas evitaria varrer tudo.
7. **Thread de gravacao (stage B)** ocupada 70-88%: e o gargalo de CPU. Os logs trazem
   "Draw pipe", "drains by reason" e "Render state memo" a cada 5 s.

## 5. Regras que continuam valendo

- Fidelidade antes de velocidade; medir antes de afirmar causa (Igor reclamou de solucoes
  "alucinadas").
- Nunca fallback de memoria da GPU para a RAM do sistema (trava o Windows de 16 GB).
- Nunca versionar arquivos do jogo; `CUSA00785` e somente leitura.
- Nao encerrar processos pelo nome global `bb-probe`.
- Igor roda o jogo e relata; nao compilar com o jogo aberto.
- Push no GitHub so com pedido do Igor.

## 6. Como seguir sem pisar no principal

O `AGENTS.md` desta pasta proibe editar o `bbport-base` principal. Sugestao: criar uma
copia nova a partir da tag `versao-60fps-2026-10-09` (como foi feito com
`bbport-iluminacao`) e entregar commits/patches para integracao, como na rodada ADDR64.
Se o Igor autorizar trabalho direto no principal, registrar isso no `AGENTS.md`.

## 7. Depois: versao 1.0 no GitHub

Pacote sem nenhum arquivo do jogo (cada pessoa usa o proprio dump), executavel, scripts e
instrucoes. A base e GPL-2.0: distribuir o executavel exige publicar o codigo-fonte (o
repositorio hoje e privado). O release so sai com o ok do Igor.
