# Port nativo do The Order: 1886 (base: BBPort)

## Pastas
- `CUSA00785/`: dump do jogo (base + patch 1.02 já aplicado). **Somente leitura, nunca vai para o git.**
- `CUSA00785-patch/`: patch 1.02 (eboot idêntico ao do jogo base).
- `bbport-base/`: clone de github.com/Supermedo/bloodborne_pc (GPL-2.0), branch nossa: `the-order`.
- `analise/`: inventário e scripts de análise (`nomes_nid.py`, `cobertura.json`, `nomes_faltantes.json`).
- `work/out/`: saída das etapas de preparação.
- MSYS2 (compilador CLANG64): `B:\msys64` (usar `MSYS2_ROOT=/b/msys64`).

## Estado (2026-10-07)
- Dump descriptografado, versão 1.02, mesmos módulos embutidos do Bloodborne (`libc.prx`, `libSceFios2.prx`).
- Etapas Python do BBPort rodam sem mudança: `prepare.py`, `link_libc.py`, `link_modules.py`,
  `content_profile.py` (139 funções ligadas como nativas, 291 ficam para o runtime).
- 355 importações; **297 já implementadas** pelo BBPort (por NID); faltam 58, 51 com nome identificado.
- GPU (libSceGnmDriver): 24/24 cobertas.
- Mudança feita na branch `the-order`: o runtime compara importações só pelo NID
  (`runtime_nid_eq` em `src/runtime.h`), porque o sufixo `#lib#mod` muda de um eboot para outro.

## Faltando (por prioridade)
1. Kernel de verdade: event flags (Create/Delete/Wait/Poll/Clear/Set), chaves de thread
   (pthread_key_create/delete, get/setspecific), sched param, condattr, Getdents, Chmod, Utimes,
   GetTscFrequency.
2. Áudio: sceAudioOutOutputs, sceAjmBatchJobRunSplitBufferRa.
3. Stubs (responder "ok"/"nada a fazer"): PlayGo, MsgDialog, SaveDataDialog, Ime, ScreenShot,
   VideoRecording, NpTrophy Destroy*, AppContent Addcont*/EntitlementKey, VideoOutSetWindowModeMargins.
4. libc (cos, sin, log, asctime, strnlen, _Exit): devem resolver pela libc nativa do jogo.
5. Sem nome ainda: T8fER+tIGgk, 5bBacGLyLOs, SfQIZcqvvms, L3XZiuKqZUM, xSxPHmpcNzY, dKadqZFgKKQ, NLq2b1jOaN0.

## Marco: menu principal (2026-10-07)
O jogo abre, mostra a abertura, a tela de título ("PRESS ✕") e o **menu principal**, que responde ao
teclado (Espaço = ✕). RAM do jogo: ~3,5–4 GB. Commit `8dba1b8` na branch `the-order`.
Ciclo de desenvolvimento: `bash iterar.sh [segundos]` (gera nomes, compila, roda, fecha o jogo, mostra onde parou).
Rastrear travas: `BB_TRACE_SEMA=1 bash iterar.sh`.

Principais correções no caminho: NID sem sufixo; event flags; stubs de serviços; fila de hooks de memória;
formatos FMASK (MSAA 4x); rejeição de descritores lixo (tile mode, formato, imagem > 512 MiB ou fora da
memória mapeada); defaults seguros de sampler; chunks de gravação de 1 MiB; uploads em lotes de 64 MiB;
teto de 256 MiB por buffer. **Sem fallback para a RAM do sistema** (travava o Windows com 16 GB).

## CPU / temperatura (2026-10-07)
O i5-10400F chegava a 100 °C: as 4 threads "Task Kernel" do jogo ficavam em loop de espera (yield) a
99,5% cada, mais 83% da thread de comandos da GPU. Correções: `runtime_relax()` (src/runtime_kernel.c:
rajada de yields/sleeps curtos dorme 0,5 ms; `BB_NO_RELAX=1` desliga para comparar) e `IdleBackoff`
(liverpool.cpp: após 1 ms sem progresso, 100 µs por volta). Resultado na tela de título: 501% → 109%
de um núcleo (9% da CPU), mesma taxa de quadros. Medir: `python analise/cpu_threads.py 10` com o jogo aberto.
Ainda: GpuCommandProcessor ~69% de um núcleo (investigar).

## Pendências conhecidas
- Avisos "SRT walker" do recompilador de shaders: descritores lidos errado → texturas faltando/erradas.
- NEW GAME ainda não testado (o mouse sobre a janela parece mexer na seleção do menu).
- MSAA vira "sem MSAA" (FMASK zerado).
- run21: 2.818 quadros; parou logo depois de um SaveDataDialog com `ReserveRecordData` (dados de um
  comando > 1 MiB por chunk, `vk_scheduler.h`). Próxima correção: cair para gravação direta quando o
  dado não couber num chunk, em vez de só aumentar o tamanho.

## Próximo passo
Compilar (`bash build.sh` no MSYS2 CLANG64) e rodar com `BB_GAME_DIR` apontando para `CUSA00785`:
o loader para na primeira importação sem implementação ("STOP: first unsupported PS4 import") e
vamos implementando uma a uma.

## Estado em 2026-10-07 (fim do dia)
- NEW GAME: save `Playthrough01` criado; cutscene inicial roda ~2 min; menu de pausa funciona.
- Queda no driver NVIDIA (nvoglv64.dll, vkCmdEndRendering): flip de CPU gravava direto enquanto a thread
  DrawRec ainda gravava → `DrainDrawPipe()` no início de `Presenter::PrepareFrame` (testar várias vezes).
- Camadas Vulkan implícitas desligadas no iterar.sh (`VK_LOADER_LAYERS_DISABLE=~implicit~`).
- Relatório de falha agora mostra nome da thread e pilha (RtlCaptureStackBackTrace); símbolos com
  `llvm-symbolizer --obj=out/bb-probe.exe 0x140000000+offset`.
- PRÓXIMO: run33 caiu com buffer de 3 GB DeviceLocal (ErrorOutOfDeviceMemory) logo após rastrear uma
  região de 3 GB. Origem provável: arena do buffer cache (`BufferCache::GetArena`, buffer_cache.cpp:418)
  ou região mapeada enorme. Limitar/espalhar a arena para caber na VRAM de 6 GB.
