# Diagnóstico F0 do serviço de coerência (Crash 4 e Ghost of Yōtei)

Data: 2026-10-08. Autor: sessão Claude Code. Branch `cpu-coherence-service`, worktree
`/home/jonathanbraga/kyty-coherence`. Complementa o `docs/ARQUITETURA-ESCRITAS-RUNTIME-2026-10-08.md`
(seções 1b e 5).

Objetivo da F0: medir **antes** de mudar código, para confirmar onde a arquitetura nova ganha.
Pela seção 10 do documento de arquitetura, este diagnóstico alimenta a revisão própria do serviço de
uploads (seção 9.8). A fase 0 das escritas runtime segue em paralelo.
Nada foi implementado nem executado por esta sessão. Os números da seção 1 vêm das capturas que o
Codex já fez.

---

## 1. O que as capturas existentes já mostram (Crash 4, PPSA02433)

**Fonte:** `_Build/crash-perf-20261007/`, gerado pelo Codex com o binário `a603778`
(build ID `0a8dd149…`):

- janelas `*-window.json`, produzidas pelo `summarize_window.py` a partir do `summary.csv` do
  `KYTY_HANG_TRACE`;
- perfil `perf-base.data`, resumido em `cpu-summary.json`.

### 1.1 Por frame (flip), por janela

| Janela | fps | GPU ocupada (ms) | GPU parada esperando a CPU (ms) | espera do CP pela GPU (ms) | faults de escrita | tempo em fault (ms) | chamadas de proteção | tempo de proteção (ms) | cópias de upload | upload (MB) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| bda-off | 18,8 | 16,5 | 33,1 | 6,5 | 170 | 17,0 | 2.205 | 26,2 | 2.486 | 67,8 |
| bda-on (batch protect) | 19,9 | 16,3 | 30,2 | 6,7 | 174 | 19,7 | 1.178 | 26,0 | 2.542 | 75,6 |
| parada, mprotect | 17,4 | 18,9 | 34,7 | 7,3 | 194 | 26,7 | 1.376 | 33,6 | 3.327 | 86,5 |
| parada, UFFD | 22,0 | 15,7 | 26,3 | 6,2 | 177 | 11,5 | 1.201 | 14,4 | 2.677 | 75,8 |
| UFFD 1024, antes | 21,7 | 15,8 | 26,9 | 6,3 | 177 | 10,7 | 1.254 | 14,1 | 2.784 | 69,3 |
| UFFD 1024, depois | 19,3 | 17,1 | 31,1 | 7,2 | 203 | 12,7 | 1.459 | 16,5 | 3.324 | 83,0 |
| UFFD 512 | 22,4 | 15,1 | 26,1 | 5,6 | 231 | 8,4 | 1.116 | 11,4 | 2.595 | 58,7 |

**Leitura:**

- A GPU fica **parada esperando a CPU 26-35 ms por frame**, contra 15-19 ms ocupada. O jogo está
  limitado pela CPU.
- O tempo de fault e de proteção soma várias threads, então não é só do Thread_Gpu. É isso que a
  seção 3.1 pede para separar.
- O UFFD corta o tempo de proteção e de fault pela metade (33,6 → 14,4 ms e 26,7 → 11,5 ms na cena
  parada), e o fps vai de 17,4 para 22,0.
- Cada frame sobe 59 a 87 MB em 2.500 a 3.300 cópias.

### 1.2 Perfil por thread

`perf-base.data` foi capturado em 2026-10-07 às 23:41 e cobre 15,0 s a 999 Hz, só ciclos de
usuário (`exclude_kernel=1`).

| Thread | Amostras | CPU de usuário em 15 s | O que domina |
| --- | --- | --- | --- |
| RenderThread 1 (guest) | 14.447 | 14,5 s | código JIT do guest (spin na fila lock-free, como medido antes) |
| Thread_Gpu | 12.078 | **12,1 s (≈80%, fora o tempo de kernel)** | ver abaixo |
| CpSequencer | 5.061 | 5,1 s | `Sequencer::WaitSlow` / `AwaitAnswer`: **esperando** |
| CP recorder | 4.236 | 4,2 s | `Ring::WaitPublished`: **esperando trabalho** |
| DrawPrep#1, DrawPrep#2 | 4.515 + 4.461 | 9,0 s | preparação |

**Topo do Thread_Gpu:**

| Símbolo | % do Thread_Gpu | ≈ s em 15 s |
| --- | --- | --- |
| `__memmove_avx_unaligned_erms` | **18,2%** | **2,2 s, uns 7 ms/frame a 20 fps** |
| `kernel` (entrada e saída de syscall e fault) | 13,0% | 1,6 s |
| `DrawPrep::Engine::ReadyHead` | 5,6% | 0,7 s |
| `std::_Rb_tree_increment` | 3,9% | 0,5 s |
| `HangTrace::RecordTransfer` | ~3,5% | **custo do próprio trace**, que estava ligado |
| `__memcmp_avx2_movbe` | 3,4% | 0,4 s |
| `BufferCache::SynchronizeBuffer` / `SynchronizeBuffersInRange` | ~3% | 0,4 s |
| `GuestAddressSpace::ProtectTransient` | ~2% | 0,2 s |
| `ObtainReadBinding`, `FindContainingUnlocked`, `ResolveSubmission`, `CollectHotPages` | ~1-1,5% cada | — |

**Conclusões:**

1. O **Thread_Gpu continua sendo o caminho crítico**: o sequencer e o recorder passam a maior
   parte do tempo esperando por ele.
2. O **memcpy guest→staging é o maior item isolado do Thread_Gpu**, com cerca de 7 ms/frame. A
   estimativa anterior, 4,3 ms (13% no perfil de 2026-10-06), fica abaixo disso. A meta da F1 sobe
   para algo como −5 a −7 ms/frame, se a cópia sair toda do Thread_Gpu, menos o custo dos guards.
   **Isso é estimativa.**
3. O `memcmp` (3,4%) e o `_Rb_tree_increment` (3,9%) também merecem atenção depois. São prováveis
   comparações de shadow (hot pages) e travessia de mapas de intervalos.

## 2. Achado no código: por que o memcpy está no Thread_Gpu nesta máquina

- O host copy do `UploadDma` (`KYTY_UPLOAD_DMA_HOST_COPY`), que levaria a cópia para um worker,
  só existe com o `UploadDma`.
- O `UploadDma` exige uma família de fila **transfer-only** (`vulkanWindow.cpp:670-689`). Sem ela,
  o `UploadDma::Create` devolve nulo (`uploadDma.cpp:51`).
- O RADV só expõe essa família com `RADV_EXPERIMENTAL=transfer_queue`, marcada pelo Mesa como
  "GFX9+, not yet spec compliant" ([envvars](https://docs.mesa3d.org/envvars.html)).
- **Resultado:** na RX 9070 XT, o memcpy sempre roda no Thread_Gpu (`bufferCache.cpp:3100-3104`,
  `2863-2866`).
- **O log de filas engana:** `VulkanFindQueueFamily` (`vulkanWindow.cpp:140-155`) para na primeira
  família que serve, então só lista a universal.
- **Não usar `RADV_EXPERIMENTAL=transfer_queue` como atalho:** a fila não segue a spec, e o host
  copy do `UploadDma` não trata as escritas do próprio CP na memória do guest. Esse é o ponto que
  a F1 resolve com os guards por faixa.

## 3. O que falta medir (F0 propriamente dita)

### 3.1 Crash 4: separar por thread

1. **Faults e proteção por thread.** Rodar com `KYTY_FAULT_MAP=1` (`faultCost.h:41-46`). A cada 10 s
   (`KYTY_FAULT_MAP_SECONDS`), no flip do CP, sai um bloco "FaultMap" com:
   - faults por thread (CP, guest, outras) e por origem (código do guest ou emulador);
   - reincidência da mesma página;
   - chamadas de proteção por thread e por direção;
   - divisão do tempo dentro do handler.

   **Pergunta:** quanto dos 11-34 ms/frame de proteção e fault cai no Thread_Gpu?
2. **memcpy por caminho.** O mesmo `perf`, com caller do `memmove`: `UploadCopies`
   (`bufferCache.cpp:3247`), o caminho reservado (`:2853-2858`) ou o `FinishBdaBatchedUpload`
   (`:3085-3137`). Isso define se a F1 cobre o `RunBdaPass`, os bindings de leitura (F2) ou os dois.
   O binário é stripped; os símbolos vêm do `kyty_emulator_clang_lld.map` do mesmo build.
3. **Escritas do CP na memória do guest por frame.** Contar DUMP_CONST_RAM, WRITE_DATA, labels
   EOP/RELEASE_MEM, GDS, flip e LOD stats. Esses são os pontos onde a F1 põe o guard
   `BeforeEmulatorWrite`. Se não houver contador para algum deles, a F1 cria um junto com o guard.
4. **O mesmo perfil sem `KYTY_HANG_TRACE`.** O `RecordTransfer` custa cerca de 3,5% do Thread_Gpu.
   Uma janela sem trace serve de controle.

### 3.2 Ghost of Yōtei (PPSA05512, patch Performance)

Não há captura recente. As últimas estão na Lixeira (`cpu-gpu-sync/yotei-*`). Os números antigos
eram 20 fps, 756 ms/s esperando a GPU, 1.187 esperas/s, 38 mil trocas de contexto/s e 3.177 faults
de escrita/s.

- Mesmo pipeline do Crash 4 (`KYTY_HANG_TRACE=1` + `summarize_window.py` + `perf` de 15 s), numa
  cena fixa.
- **Pergunta:** os 756 ms/s de espera da GPU vêm de onde? Colunas do `summary.csv` que ajudam:
  `master_gpu_wait_us`, `cp_gpu_wait_us`, `publication_wait_us`, `pred_flush_wait_us`,
  `readback_us`, `done_avg_us`. Os motivos detalhados estão em `FrameWait::GpuWait*`
  (`profiler.h:1262-1276`).
- Com `KYTY_FAULT_MAP=1`, também a divisão dos 3.177 faults/s por thread.

### 3.3 Protocolo

- Quem roda os jogos é o usuário. A sessão Claude não abre jogo.
- Mesmo binário (`Source build` no log), mesma cena, cache aquecido (descartar a primeira abertura
  depois de cada rebuild).
- Preset U59 como está agora, com `KYTY_BDA_BATCH_PROTECT=1` e `KYTY_UFFD_WP=1` do trabalho do Codex.
- Janelas de 15-20 s, alternando por `KYTY_LIVE_FILE` quando o switch for live.

## 4. Pedidos ao Codex

1. **Validar a seção 1 e completar a 3.1:** o `summary.csv` e as janelas já são seus, e o binário e
   a cena estão aquecidos aí. Pode rodar `KYTY_FAULT_MAP=1` e o `perf` com caller na próxima
   captura do Crash 4?
2. **Confirmar qual janela corresponde ao `perf-base.data`** (23:41), para cruzar fps e perfil.
3. **Arquivos:** a F1 vai mexer em `bufferCache.cpp` (sites de upload `2845-2868`, `3100-3260`), em
   `stagingCopier.*`, em `commandScheduler.h` (um slot de `SubmitDependency`) e nos escritores de
   memória do guest em `graphicsRun.cpp`. O `RunBdaPass` (`bufferCache.cpp:4563`) e o
   `pageManager.cpp` são seus pela batch protect. Combinamos que a F1 não toca o `RunBdaPass` nem o
   `pageManager.cpp` (isso é a F3, depois de você), certo?
4. ~~As perguntas da seção 4 do documento de arquitetura~~: respondidas pelo Codex na seção 9.6.
   O UFFD bloqueia só escrita (página pendente usa `NoAccess` via `mprotect`), o `RunBdaPass` não é
   hook de settle, e não há trabalho do Codex em andamento em `faultManager`/`bufferCache`.

As respostas podem ir no fim deste arquivo, ou num arquivo novo ao lado.
