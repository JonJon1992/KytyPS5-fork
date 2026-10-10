# chenxiao07/KytyPS5 v20261010: o que serve para a guest-sync-release-mem

Data: 2026-10-10. Release `v20261010` = commit `54bbdf7d` (build Windows para Demon's Souls, i9-14900K + RTX 5090).
O commit foi buscado para `refs/chenxiao/v20261010`. Nada foi portado, compilado ou executado.

**Base:** `guest-sync-release-mem` em `701751b1` (busca no working tree, que tem diff de outra sessão; os fatos
citados também valem no `HEAD`). **Escopo:** só o que é novo desde a triagem anterior (`docs/PORT-CHENXIAO-2026-10-07.md`, até `4f7bacb4`). A regra do
usuário continua valendo: nada substitui as nossas otimizações (BDA, CP sequencer/recorder, DrawPrep/draw runs,
bindless, strides em runtime, program cache).

**Conclusão curta:** quase tudo o que a release anuncia já existe aqui com outro desenho, ou não se aplica a
Linux/RADV/RDNA4. Não há porte de código urgente. Há dois A/B sem código: `KYTY_BDA_SHARED_BLOCKS=1` e
`KYTY_PIPELINE_FAST_FIRST` no RADV.

## 1. Os 63 commits de `4f7bacb4..54bbdf7d` (sem merges, 2026-10-08 a 2026-10-10)

| Tema | Qtd | Commits |
| --- | ---: | --- |
| AMD / compatibilidade | 3 | `2bf8c99b` (wave64 de CS), `894fda03` (build sem otimização só na NVIDIA), `534183f7` (diag `--amd --validate`) |
| VRAM e pouca RAM | 4 | `45beec32` (coletor não apagava nada), `ca61c070` (GC nos flips da GPU), `4a59e94c` (<12 GiB: VRAM para render targets), `5cf2f6d1` (<24 GiB de RAM: warmup não traduz) |
| Pausas: texturas e upload | 7 | `06e2787e` (só os mips da view), `f17993c1` (reabastece o staging antes do submit), `21cfed4a` e `e7bf421a` (escritas parciais), `8fe29414` (camadas de depth), `3e075254` (staging grande em threads auxiliares), `af29b93d` (buffers BDA nos blocos do VMA) |
| Pausas: áudio e escalonamento | 2 | `2f9a63da` (APR com arquivo mantido aberto), `2ac66300` (prioridade da render thread) |
| ZArchive / caminhos não-ASCII | 1 | `49adfb79` |
| Crash de draw indireto | 1 | `11dee8cf` |
| Present | 2 | `a3014426` (present sem o lock do renderer), `6f4bc3cd` (acima da taxa do monitor, mostra o último frame) |
| Table mode / native XPR | 12 | `42677bea`, `cf1ccfef`, `9cd12b4d`, `6fe10727`, `d7c51a57`, `ccff780b`, `3c4f4a99`, `3ee9be77`, `90e561e2`, `ea973702`, `78b35c88`, `5d34158a` |
| Micro-otimizações do thread da GPU sobre subsistemas do fork (readback, write log, alias, proteção, lookups) | 17 | `e953c484`, `55a8a669`, `9527095f`, `2a82061c`, `14b28114`, `975a1d5e`, `09bb6560`, `05287b24`, `fc692a33`, `4704d19a`, `19256455`, `4b7f00c4`, `d5d0b5b4`, `5c0833a4`, `e9996ef7`, `af46340f`, `c9d4d4be` |
| Shader lane-local (hosts de 32 lanes) | 2 | `9907780e`, `9bb47e15` |
| Precompile / PGO | 2 | `9bf8786e`, `75651a5c` |
| Diagnóstico / docs | 10 | `b22d0f02`, `a3f5141c`, `541d5d67`, `7a758570`, `4d6b7b8b`, `805a8629`, `f4c16cba`, `ec73aa62`, `9861e532`, `54bbdf7d` |

**Aplicabilidade:** `git apply --check` de `git format-patch -1` de cada um contra o `HEAD` (índice temporário;
working tree intocado): **0 de 63 aplicam**. 35 tocam arquivos que não existem aqui (`src/local/*`,
`gpuResourceManager.*`, `speculation.cpp`, `staticPrecompile.inc`, docs dele). Todo porte seria reescrita à mão.

## 2. Itens da release contra o nosso fork

| Item da release | chenxiao | Nosso fork | Estado |
| --- | --- | --- | --- |
| RDNA2 travava no driver; CS wave64 "como na NVIDIA" fora do RDNA3+ | `2bf8c99b` | `DeviceCompat::ComputeSubgroupSize` exige o wave do guest no AMD. A RX 9070 XT (`1002:7550`) está na lista RDNA3+ dele, então a política é a mesma | Não se aplica ao RDNA4 (3.1) |
| Pipelines otimizados antes do primeiro uso fora da NVIDIA | `894fda03` | `KYTY_PIPELINE_FAST_FIRST` (padrão off; `=1` no `tools/u59-preset.json`) | Temos o mecanismo; política diferente (3.1) |
| Pausas de 0,2-0,5 s de streams de áudio lidos aos pedaços | `2f9a63da` (só Windows; outras plataformas mantêm abrir/ler/fechar) | `AprHostFiles::HostFilePool` (`d3e0e8c5`, 2026-10-02) mantém os handles abertos | **Já temos** |
| Texturas em streaming sobem só os mips desenhados | `06e2787e` | `KYTY_TEXTURE_RESIDENT_MIPS` (padrão on, `aa8eac1e`, 2026-09-27): registra, vigia e sobe só os níveis que as views podem amostrar | **Já temos** (3.3) |
| Textura reescrita antes do upload não sobe duas vezes | `f17993c1` | Não temos. Já avaliado e recusado em `PORT-DEMON-2026-10-09.md` (sem prova de sincronização) | Ausente, de propósito (3.3) |
| Render thread acima das threads do jogo | `2ac66300` | `MakeCriticalThread` dele é vazio fora do Windows | Não se aplica (3.3) |
| VRAM para render targets primeiro (<12 GiB) | `4a59e94c` | Corrige um comportamento da NVIDIA, que falha a alocação em vez de paginar. A placa tem 16 GB, e o amdgpu move para GTT | Não se aplica |
| Coletor não liberava nenhuma imagem | `45beec32`, `ca61c070` | O GC por budget e o idle passam por cima das imagens mantidas sem contá-las. O coletor legado tem o mesmo bug | Parcial (3.4) |
| <24 GiB de RAM: warmup não traduz | `5cf2f6d1` | O warmup é dele; o nosso precompile é outro. Esta máquina tem 32 GB | Não se aplica |
| ZArchive (`.zar`) | `49adfb79` | Upstream #724 (`4b0de1b6`) já está no `HEAD` (`src/common/archive.*`, `Common::File`, então o APR também lê `.zar`) | **Já temos** a base; as 4 lanes de descompressão dele não importam (não usamos `.zar`) |
| Pastas com nome não-ASCII | `49adfb79` | Problema da code page ANSI do Windows; no Linux os caminhos são UTF-8. `Common::PathFromUtf8` existe aqui | Não se aplica ao Linux; não verificado no Windows |
| Crash em draw indireto (visto no Linux) | `11dee8cf` | A ordem já é a certa (3.2) | **Não temos o bug** |

## 3. Pontos de atenção

### 3.1 AMD

**`2bf8c99b`:**
- Com subgroup size control, CS wave64 só roda nativo (`requiredSubgroupSize = 64`) em AMD RDNA3+. A detecção é por
  device ID: `0x7440-0x75FF`, `0x15BF`, `0x15C8`, `0x150E`, `0x1586`, `0x1114`.
- Nos outros, o CS é traduzido para 32 lanes, com duas lanes do guest por invocação (o caminho da NVIDIA).
- `KYTY_COMPUTE_WAVE64=0/1` força um ou outro.
- Gatilho: uma RX 6700 XT com Adrenalin 26.8.1, no Windows, travou dentro do driver ao criar o ~21º pipeline de
  compute. O SPIR-V valida (`534183f7`), então é bug de driver, sem causa confirmada.
- **No RDNA4/RADV não muda nada:** a 9070 XT cai em "nativo", que é a nossa política (`WAVE-SIZE-2026-10-09.md`). Não
  há conflito.
- Para um usuário RDNA2 no Windows, bastaria um override equivalente. Atenção: o nosso `ComputeSubgroupSize` exige o
  wave do guest sempre que `compute_wave64` é verdadeiro, então o override teria de mudar também o tamanho exigido. É o
  hunk de `shaders.cpp` do commit dele.

**`894fda03`:**
- O primeiro build sem otimização (`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`) fica só na NVIDIA; os outros
  compilam otimizado antes do primeiro uso. `KYTY_PIPELINE_FAST_BUILD` força.
- O suspeito do crash da RDNA2 é esse build sem otimização. Não confirmado: o precompile, que compila otimizado,
  rodou naquela máquina sem crash.
- **O nosso preset liga `KYTY_PIPELINE_FAST_FIRST=1` em qualquer GPU.** A medição dele (Sky Garden: 1,38 s para
  0,31 s) foi numa RTX 3090.
- No driver AMD do Windows o bit não serve: `performance-amd.md` mediu −4% a −8% de tempo de compilação ("Dead end:
  unoptimized first compiles").
- No RADV não há medição. É provável que o RADV respeite o bit (NIR/ACO pulam otimizações), mas isso não foi
  verificado aqui.
- **Ação sem código:**
  - A/B de `KYTY_PIPELINE_FAST_FIRST=0/1` com cache frio, olhando `fast_ns`/`optimize_ns` dos contadores de fast-first
    e as travadas de primeiro uso;
  - num perfil Windows+AMD, deixar fast-first off.

### 3.2 Crash do draw indireto (`11dee8cf`)

- **O bug no chenxiao:** o `DrawIndex` testava `state.vs_input_info.stage.program->stage == Mesh` antes do
  `RefreshShaders`. O teste lia o programa do draw anterior, ou um ponteiro nulo num estado novo.
- **No nosso código a ordem está certa:**
  - `PrepareDrawRenderState` chama `RefreshShaders` e descarta o draw sem programa de vértice;
  - o teste de mesh do caminho indireto nativo e do `DrawIndex` vem depois.
  - Linhas no `HEAD` de `renderDraw.cpp`: 2154-2161, 4311→4319 e 4018→4023.
- O `ExecutePreparedDraw` usa um estado já preparado. **Não temos o mesmo bug.**

### 3.3 Pausas de textura e de áudio

- **Áudio (`2f9a63da`):**
  - Já resolvido pelo pool de handles do APR.
  - No Linux, o readahead do kernel também detecta fluxo sequencial entre aberturas (context readahead), então o
    ganho dele era sobretudo do Windows.
- **Mips desenhados (`06e2787e`):**
  - É o nosso resident mips. A diferença: quando o jogo baixa o `BASE_LEVEL`, ele sobe só os tiles sujos dos níveis
    novos, e nós subimos todos os níveis residentes (`MarkResidencyDirty`).
  - O nível novo é o maior (~3/4 dos bytes), então a diferença é de ~25%, só nesses eventos. Estimativa; não medido.
- **Escritas parciais (`21cfed4a`, `e7bf421a`, `8fe29414`):**
  - Dependem da infraestrutura `PartialDirty` dele, com grânulos de 1 MiB.
  - Os padrões medidos são do Demon's Souls: o pool de shadow maps de 80 camadas em memória esparsa, remapeado uma
    camada por vez.
  - O nosso equivalente (`KYTY_TEXTURE_PARTIAL_UPLOAD`) está desligado pelo bug U44 do Astro Bot (mips grossos
    velhos).
  - O nosso `UnmapMemory` libera a imagem inteira mesmo quando a GPU a escreveu. Só importaria se os nossos jogos
    desmapeassem imagens renderizadas em parte; não há evidência disso.
- **`f17993c1`:**
  - Reaproveita o staging de um upload ainda não submetido; draws anteriores passam a ver bytes mais novos.
  - Recusado antes por falta de prova. Antes de reconsiderar, medir se Crash/Yōtei sobem a mesma imagem várias vezes
    no mesmo command buffer aberto. Não existe contador para isso; seria um contador pequeno.
- **`3e075254`:** divide cópias de staging ≥1 MiB entre 7 threads, para diluir o custo de primeiro toque nas páginas
  do backing (~1,2 µs/página no Windows). No Linux esse custo é outro (minor fault no memfd) e não foi medido aqui.
- **`2ac66300`:** só Windows (`THREAD_PRIORITY_HIGHEST`). No Linux, subir a prioridade do Thread_Gpu exige
  `CAP_SYS_NICE`. Antes de qualquer ideia, medir o `run_delay` do Thread_Gpu em `/proc/<pid>/task/<tid>/schedstat`.
  Fixar o RenderThread num núcleo já piorou (`RENDERTHREAD-2026-10-09.md`).

### 3.4 Coletor de VRAM (`45beec32`, `ca61c070`)

- **O bug no chenxiao:** o coletor pegava as 10/20/40 imagens menos usadas e contava contra o orçamento também as que
  precisa manter (tiled escritas pela GPU, depth com stencil). Elas ficam na frente da LRU, então nada era apagado.
- **O nosso caminho do preset já evita isso.** Com `KYTY_VRAM_GC_BUDGET=1` e `KYTY_VRAM_IDLE_FRAMES=600`, o
  `RunBudgetGarbageCollector` e o `RetireUnusedImages` testam `IdleRetirable` e passam por cima das imagens mantidas
  sem contá-las (comentário em `textureCache.cpp`, `RetireUnusedImages`).
- **O coletor legado tem o mesmo defeito.** É o lambda `collect` de `TextureCache::RunGarbageCollector`, que roda sem
  budget GC e sem `KYTY_IMAGE_CACHE_POLICY=pressure` (por exemplo, rodando pela CLI sem o preset). Ele decrementa
  `deletions` antes de pular `depth_id` e as tiled sujas.
- **Correção possível:** pequena (passar sem contar) ou, melhor, tornar o budget GC o padrão. Cuidado: com o defeito
  corrigido, o gatilho legado ((budget − 8 GiB)/2 ≈ 3,5 GiB numa placa de 16 GB) apagaria texturas cedo. Foi o que o
  chenxiao viu e por isso subiu o gatilho.
- **`ca61c070`:** não se aplica. Aqui `TextureCache::AdvanceFrame` roda em todo flip concluído pela `FlipQueue`,
  inclusive os da GPU (`SubmitFlipFromGpu` → `FlipQueue::Prepare`). O GC roda após cada submissão concluída
  (`graphicsRun.cpp`).

## 4. Fora das notas da release

- **Table mode / native XPR (12)** e **readback, write log, alias e proteção (17):** dependem de subsistemas que só o
  chenxiao tem, ou duplicam os nossos. Exemplos: `KYTY_IMAGE_TRANSIT_SKIP` já cobre o `5c0833a4`; a proteção dele é
  `VirtualProtect`, a nossa é UFFD. **Não portar.**
- **Lane-local (`9907780e`, `9bb47e15`):** só ajuda hosts de 32 lanes; no AMD o wave64 é nativo. Já recusado em
  `PORT-DEMON-2026-10-09.md`.
- **`c9d4d4be`:** continuação do `bb82105c` (seção 4 do doc anterior). Só faz sentido junto com ele.
- **`5d34158a`:** salva o driver cache durante a sessão. No RADV o cache de disco do Mesa já persiste por pipeline.
  Só Windows.
- **Ideias pequenas para o Thread_Gpu** (o nosso gargalo). Todas são reescrita à mão e de ganho pequeno:
  - **`a3014426`:** o nosso `Present` (`swapchain.cpp`) grava e submete o blit segurando o mutex do renderer. O
    Thread_Gpu pega esse mutex em todo `DrawIndex`, `DrawAuto` e indireto. O chenxiao mediu ~110 µs/frame de espera,
    menos de 0,2% dos nossos ~68 ms no Crash 4. O overlay do sistema pode depender do estado do renderer.
  - **`e953c484`:** lock assimétrico (Dekker com `FlushProcessWriteBuffers`) para tirar 2 atômicos por draw. No Linux
    o equivalente é `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)`. Ganho estimado abaixo de 1% do Thread_Gpu no Zen 3
    (especulativo). O risco de ordenação de memória é alto.
  - **`4704d19a`:** o `CommandProcessor::Reset` zera os 48 KiB de constant RAM a cada submissão com
    `reset_processor`. Uma marca de nível máximo de escrita reduziria isso; o ganho é ínfimo (µs por submissão).

## 5. Recomendação

| Item | Ganho esperado (RX 9070 XT, Crash/Yōtei/Astro) | Custo | Risco | Recomendação |
| --- | --- | --- | --- | --- |
| A/B `KYTY_BDA_SHARED_BLOCKS=1` (equivale a `af29b93d`) | Menos `vkAllocateMemory` ao entrar em área nova. No Windows, 30% das amostras nos frames de travada; no Linux o ioctl custa menos (especulativo) | Nenhum (switch existente) | Baixo-médio (VRAM presa em blocos do VMA) | **Testar agora** (sem código) |
| A/B `KYTY_PIPELINE_FAST_FIRST=0/1` no RADV (`894fda03`) | Incerto: menos travada de primeiro uso, ou nada | Nenhum | Nenhum | **Testar agora**; desligar no perfil Windows+AMD |
| `2bf8c99b`: override de wave64 em CS | Zero no RDNA4 | Pequeno | Baixo | Não portar (só se aparecer usuário RDNA2 no Windows) |
| `11dee8cf` | Zero (não temos o bug) | — | — | Não portar |
| `2f9a63da`, `06e2787e`, `49adfb79` | Zero (já temos) | — | — | Não portar |
| `f17993c1` | Desconhecido: depende de re-upload no mesmo command buffer | Médio | Médio (bytes vistos por draws anteriores) | Depois, só se um contador novo mostrar re-uploads |
| `21cfed4a` / `e7bf421a` / `8fe29414` | Baixo nos nossos jogos (padrões do DeS) | Alto (infra parcial dele) | Médio | Não portar |
| Coletor legado (ideia do `45beec32`) | Só fora do preset | Pequeno | Baixo | Depois: budget GC como padrão |
| `ca61c070`, `4a59e94c`, `5cf2f6d1` | Zero (16 GB de VRAM, 32 GB de RAM, frames já contam) | — | — | Não portar |
| `2ac66300` (prioridade) | Desconhecido no Linux | Pequeno | Médio | Depois: medir `run_delay` do Thread_Gpu antes |
| `3e075254` (staging paralelo) | Especulativo | Médio | Baixo-médio | Depois: medir o custo de primeiro toque no Linux antes |
| `a3014426` (present sem lock) | ~0,1 ms/frame do Thread_Gpu | Médio-baixo | Médio | Depois |
| `e953c484` (lock assimétrico + membarrier) | <1% do Thread_Gpu (especulativo) | Médio | Alto | Não portar agora |
| `4704d19a` (constant RAM) | Ínfimo | Pequeno | Baixo | Depois (opcional) |
| Table/native XPR, readback/write log/proteção, lane-local, precompile, PGO, diag | — | — | Conflitam com a nossa arquitetura | Não portar |
