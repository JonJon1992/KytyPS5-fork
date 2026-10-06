# graphicsRun.cpp/.h — caminho quente da Thread_Gpu, 2026-10-05

## Ponto de partida (medições já documentadas)

- Com o preset U59 (`KYTY_CP_SEQ=1`), o resolver (`Thread_Gpu`) é o gargalo, a ~3,35 µs por
  draw; o sequenciador, que roda o parse PM4, passa ~80% do tempo esperando espaço na janela
  ([CLAUDE-DRAW-HOTPATH-2026-10-04.md](CLAUDE-DRAW-HOTPATH-2026-10-04.md)).
- Sem o preset, o parse PM4 na própria thread da GPU era ~3% dela. Os custos grandes do resolver
  estão fora deste arquivo: caminho de buffers (~21%) e `memmove` (~8%) no perfil com preset, e
  `PrepareBda` (22,7% da thread) dentro de `ExecutePreparedDraw` num perfil anterior com pilhas
  ([CLAUDE-ORDEM-DE-TAREFAS.md](CLAUDE-ORDEM-DE-TAREFAS.md)).
- `vkGetSemaphoreCounterValue` custa ~1,1 µs no RADV (ioctl `drmSyncobjQuery2`); o preset já limita
  essa consulta em `PopOperations` (`KYTY_PENDING_REFRESH_US=200`).

Conclusão: dentro de `graphicsRun.cpp` sobram custos fixos por pacote/op e travas compartilhadas,
não o grosso do tempo por draw. As mudanças abaixo removem esse trabalho sem mudar o
comportamento padrão. **Não houve medição de FPS.**

## Medição nova: custo da consulta ao timeline semaphore neste PC

Sonda Python/ctypes sobre `vulkan-1.dll` (device temporário; a chamada vazia
`ntdll!RtlCompareMemory(p, p, 0)` com a mesma forma de argumentos é descontada; 9 rodadas de
200.000 chamadas, mediana). GeForce RTX 2060, driver NVIDIA `0x9a438000`, Windows 11:

| Estado do semaphore | Trampolim do loader | `vkGetDeviceProcAddr` |
|---|---:|---:|
| Ocioso | 63 ns | ~0 ns |
| Submissão pendente (espera um gate do host) | 38 ns | 125 ns |
| Sinalizado | 45 ns | ~0 ns |

O ruído do ctypes é da ordem de ±50 ns; a conclusão é "abaixo de ~0,1 µs" (leitura em modo
usuário), contra ~1,1 µs no RADV. Esta máquina não tem a RX 9070 XT citada em
`performance-amd.md`.

## Alterações

| Onde | Antes | Depois | Quem ganha |
|---|---|---|---|
| `ProcessPacket` | `call GuestGpu::ProcessCommands` fora de linha em todo pacote sem draw-prep pendente | Carrega `m_pending_commands` e só chama com comando pendente (a mesma condição do laço de `ProcessCommands`) | Thread da GPU sem `KYTY_CP_SEQ` |
| `CountPacket` | `call CpSeq::PacketHashing` (cpOps.cpp) em todo pacote | Valor fixo do processo lido uma vez (estático local); o caminho quente testa só o byte da guarda | Sequenciador e thread da GPU |
| `HasRunnableComputeWork` | `m_queue_mutex` + varredura de 56 filas a cada chamada | Sai sem trava quando nenhuma submissão de compute está na fila (`m_compute_queued`, mantido nos 4 pontos que mudam `m_queues`) | Fatia gráfica (a cada `KYTY_GFX_SLICE_DRAWS` draws) e resolver faminto (depois do spin inicial, a cada 64 pausas); menos disputa com `Enqueue`/`SendCommand` das threads do jogo |
| `NotifyProgress` | `xchg` (escrita travada) após toda operação do completion runner | Leitura simples antes; `xchg` só com fila bloqueada | Completion runner; a linha deixa de ser escrita à toa |
| `GuestGpu` (layout) | `m_pending_commands` dividia a linha com `m_commands`/`m_deferred_labels` (libstdc++: offset 4720) e, na STL da Microsoft, com `m_in_flight`, escrito em toda admissão | `alignas(64)`: sozinho na linha 4736–4799, igual nas duas STLs | Thread da GPU, que lê o campo a cada pacote/op |
| `MaybeFlushIdleGpu` | Com a GPU atrasada, consulta o timeline a cada 4º draw | Igual por padrão; `KYTY_IDLE_FLUSH_REFRESH_US=n` (live switch) limita também a uma consulta por n µs | Linux/RADV; nada a ganhar neste driver NVIDIA (tabela acima) |

`sizeof(GuestGpu)`: 5408 → 5504 bytes; alinhamento 8 → 64 (alocado por `std::make_unique`, com o
`new` alinhado do C++17).

## Validação

- `graphicsRun.cpp` (HEAD e novo) compilado com as flags do alvo `kyty_emulator` de
  `_Build/linux` (Clang 18, `-O3 -Wall`), fora do diretório de build: 0 warnings nos dois.
- As 14 unidades que incluem `graphicsRun.h` passam em `-fsyntax-only`; os 9 warnings são antigos
  e de outros arquivos (`-Wreturn-type`, `imageView.h`).
- Desmontagem HEAD × novo: `ProcessPacket` passa a testar `0x1280` (o `m_pending_commands`
  isolado) antes de `ProcessCommands`; as 3 chamadas a `CpSeq::PacketHashing` ficam só sob
  `__cxa_guard_acquire`; `HasRunnableComputeWork` testa `0x1314` (`m_compute_queued`) antes de
  `Mutex::Lock`; `NotifyProgress` testa o byte antes do `xchg`; `MaybeFlushIdleGpu` só lê o relógio
  com o switch ligado.
- **Não rodaram:** os testes de GPU que cobrem esse caminho (`shader_recompiler_compute_tests
  --gpu-command-lane-only` e `--cp-seq-only`: filas de compute, frame fence, fatias, sequenciador).
  O lavapipe do WSL não tem fragment barycentrics e atomics de 64 bits em LDS, e o Windows desta
  máquina está sem compilador C++.

## O que medir

1. Rodar `--gpu-command-lane-only` e `--cp-seq-only` num build com GPU.
2. No jogo, Linux + AMD, mesma cena e câmera parada: A/B no mesmo processo com
   `KYTY_IDLE_FLUSH_REFRESH_US` em 0, 50 e 200 via `KYTY_LIVE_FILE`. Comparar µs/draw do resolver,
   `IdleFlushes`/`IdleFlushesInPass` e a ocupação da GPU (um intervalo longo atrasa o flush antecipado).
3. As outras mudanças não têm switch: comparar contra o binário anterior. O efeito esperado é
   pequeno (algumas instruções e uma chamada por pacote; travas evitadas), abaixo do ruído de FPS.

## Rodada 2: caminho por draw do resolver

Mapeamento por três leituras do código (bufferCache, descriptors/renderDraw, memória/backing).
Itens que o preset já resolve ficaram de fora: leitura de stream sem trava (`KYTY_CP_COMMIT=all`),
transferências com uma busca (`KYTY_FAST_BACKING_TRANSFERS=1`), `RecordTransfer` (hang trace
desligado).

| Onde | Antes | Depois |
|---|---|---|
| `descriptors.cpp` `UploadShaderData` | O acerto do último upload do site (caminho padrão) retornava sem limpar `fresh`, ao contrário dos outros três acertos | `reused(...)`: o push-descriptor shadow compara e o reuso de descriptor sets procura (~1.212 acertos/flip no Sky Garden, CHANGE-CATALOG) |
| `bufferCache.cpp` `SynchronizeBuffer` | Leituras texel (`buffer_load_format`, vertex fetch) nunca usavam o atalho relaxado: trava por região, scratch e coleta vazia em todo draw | Mesmo atalho das leituras não-texel; só resta `SynchronizeBufferFromImage`, como após uma coleta vazia |
| `bufferCache.cpp` reserva de staging (escrita) | `IsRegionCpuModified` com trava por região | `QueryCpuDirtyRelaxed` na thread da GPU; região ausente usa a trava; uma página perdida é coletada sob as travas |
| `bufferCache.cpp` `UploadCopies` | `GuestBackingAlias` (mutex + `std::map`) por cópia do DMA host copy | Uma busca para o span das cópias; fallback por cópia se o span cruza mapeamentos |
| `renderDraw.cpp` `DrawRunAcquireCandidate` | Duas aquisições do lock do texture cache por draw candidato | Uma (`DrawRunImagesChangeLocked`) |
| `graphicsRun.h` `IsGpuThread` | Chamada fora de linha, até 4 por binding | Inline (`static inline thread_local`) |
| `emulatorConfig.h` | `GraphicsDebugDumpEnabled`/`GetPrintfDirection` fora de linha, ~10–15 por draw | Inline, copiados em `Initialize`/`Load` |
| `memoryAddressSpace.inc` | Leituras sem trava até 256 B | Até 512 B (lote de V# de vértice, `shader.cpp`) |

Validação: as 210 unidades do `kyty_emulator` compilam com as flags de `_Build/linux` (0 erros;
os 28 warnings são antigos) e o executável liga (build privado em `/tmp`, membros `emulatorConfig`
e `log` da `libcommon.a` recompilados). Nenhum teste de GPU rodou (mesmos limites acima).

Estimativa, **não medida**: Astro (CP-bound, ~8.000 draws/frame) −0,10 a −0,35 µs/draw (+3% a
+10% fps); Crash < 1% (o custo dele não é fixo por draw). Contadores para conferir no jogo:
`TrackerRelaxedSyncSkips`, `DescriptorPushesAvoided`, `DescriptorPushMiss*`,
`DescriptorSetReuse*`, `BackingMapCacheHits/Misses`.
