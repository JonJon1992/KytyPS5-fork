# Triagem do fork Senaxx/KytyPS5 (`wolverine`) — 2026-10-10

Pedido do usuário depois da triagem do IDXTRI (`docs/PORT-IDXTRI-2026-10-10.md`). Base local:
`guest-sync-release-mem` @ `cd010298` mais a árvore de trabalho; para bindless, o worktree
`kyty-bindless-wt` (`bindless-cow` v12–v15).

- **Refs:**
  - `refs/senaxx/wolverine` (`48028863`, 2026-10-09);
  - `refs/senaxx/main`, que é espelho do upstream. Ele tem 178 commits que não temos, mas isso é merge do upstream e fica fora daqui.
- **Escopo:** 176 commits só do `wolverine` (174 do Senaxx), de 2026-09-27 a 2026-10-09.
  - Em 2026-10-05 portamos à mão um snapshot antigo desse branch (`6b3fbc83`, que não existe mais): shader functions, traps, BVH, FLAT e o bindless opt-in. Por isso o patch-id não serve para comparar, e a comparação foi feita lendo o código.
- **Fora da triagem** (ferramentas e afins): ~33 commits de profiler/Tracy, `shader_batch_tool`, testes, CI, rebases e dumps.
  - O `3ce5a5da` (AvPlayer) já entrou como `705398d1`.
  - O `f0dc75c4` (Tracy só em localhost) só importa se usarmos Tracy.

Tabelas completas por commit, com a evidência `arquivo:linha` do nosso lado, em `docs/port-senaxx-2026-10-10/`:

- `A-shader.md` (45 commits)
- `B-bindless-texturas.md` (25)
- `C-memoria-coerencia.md` + `C-srt-detalhe.md` (45)
- `D-pipelines-audio.md` (29)

**Ressalvas sobre esta triagem:**

- É só leitura: ninguém compilou nem rodou nada.
- Quase nada aplica limpo. `git apply --check` só passa em `28b1e6c8`, `f91035a9`, no trecho Opus de `4e5f683a` e em `f1313805` (este aplica mas não compila). O resto é adaptação.
- Os números do fork vêm de Windows/NVIDIA (alguns da 9070 XT no Windows), no Wolverine.

## Resumo

| Grupo | Portar | Já temos | N/A | Pular |
|---|---|---|---|---|
| A shader/decoder/mesh | 13 | 22 | 9 | 2 |
| B bindless/texturas | 6 | 14 | 4 | 4 |
| C memória/coerência/readback | 12 | 16 | 6 | 11 |
| D pipelines/áudio/diversos | 10 | 12 | 3 | 3 |

## Prioridade 1 — desempenho na Thread_Gpu (Yotei e Crash 4)

1. **Bindless pelo "engine method" (`c5a6961b`, com `4b0ffc1f`): levar as ideias para o `bindless-cow`, não o código.**
   - **Como o Senaxx faz:**
     - observa as páginas da heap contra escrita da CPU;
     - por submissão, só compara de novo as entradas das páginas escritas;
     - troca a geração global por uma lista de imagens alteradas, alimentada só por imagens com referência bindless;
     - no commit, transiciona só o que mudou.
   - **O que levar:**
     1. Uma marca por imagem referenciada (`AddImageReference`) e uma lista de mudanças por imagem. Isso substitui a varredura `TryRepeatEachKey` e os resets da geração global, que hoje anulam o `TICK_REPEAT` (24–75k bumps de alias owner a cada 10 s, quase todos de imagens fora de heap).
     2. Um bit de heap suja, ligado por fault de escrita da CPU, por escrita da GPU (bits sujos da coerência, que o Senaxx não tem) e por unmap. Heap limpa reaproveita a região publicada.
     3. Uma lista de commit por heap, com um toque de LRU por heap por frame.
   - **O que não levar:**
     - o settle preguiçoso como padrão, porque o Yotei cria ~500 heaps/s e cada heap nova amostraria o placeholder por 1–2 frames;
     - o fork também não tem stride de registro (o Yotei usa 440/872 B), nem rastreia escrita da GPU, nem CoW.
   - **Estimativa** (a partir das fatias do perfil, não medida): atacaria ~30–35% da Thread_Gpu. O teto realista é +25–50% de fps no Yotei (~9,5 → 12–14), porque as esperas de sync (25–30%) continuam.
2. **Preparar dispatches à frente da Thread_Gpu (ideia de `6632a241`).**
   - O nosso DrawPrep já prepara draws diretos; por isso `MaterializeResources` é só 1,8% no Crash 4.
   - Mas `DISPATCH_*` e draws indiretos são cercas de janela (`packetClass.h:88-89,145-151`). No Yotei, `DispatchDirect` toma 24% da Thread_Gpu, 16% dele em `MaterializeResources` (1269 dispatches/frame).
   - O scanner do Senaxx cobre dispatches, inclusive as filas async-compute, e confere na hora de usar que os bytes continuam limpos. O verify dele deu 0 divergências e 86% dos walks atendidos.
   - Proposta: estender o DrawPrep aos dispatches com o ReadSet e os certificados que já existem, sem criar um segundo CP.
3. **`f1313805`, ~4 linhas (risco baixo).**
   - `DownloadBufferCopies` (`bufferCache.cpp:1107`) e o readback antecipado (~3197) usam `command.Handle()`. Isso drena o recorder do CP na Thread_Gpu antes de cada readback.
   - Gravar pelo `Sink()`. No fork, os drenos caíram de 3650 para 58 a cada 5 s.
4. **Readback assíncrono quando quem escreveu está na gravação atual (resto de `f7f885ab`).**
   - Hoje drenamos na Thread_Gpu nos casos CurrentWriter, Unbounded e Other (`bufferCache.cpp:1904-1911,1965-2015`), ~110 ms/s no Yotei.
   - Montar sobre o `EarlyReleasedDownload`, com duas lições do fork:
     - pegar o tick depois de gravar as cópias (`2d421154`);
     - uma entrada por download (`402a830e`).
   - `KYTY_READBACK_SIDE_WRITES` (só no worktree bindless) já cobre parte disso: +2%.
5. **Slots planos de SRT preenchidos pela GPU (`0845bead`).**
   - Quando a GPU escreveu os bytes de slots que só o shader lê, a cópia para o SRT plano é feita na GPU.
   - Ataca ~85–100 ms/s de `HandleFault` vindos de `SrtWalker::ReadRawWord` no Yotei, sem mudar o codegen e sem o custo de `PrepareBda` que derrubou o nosso teste v19.
6. **Medir antes de portar:**
   - `19b6c5cf`: emular no fault handler as loads de bytes que a GPU não escreveu. Conflita com `m_gpu_modified_ranges`, que é só da Thread_Gpu.
   - `df053d75`: bitmap de escrita da GPU.
   - `0b97a337`: clamp por spans mesclados. Ver antes `ClampRangeMemoMisses`.
   - `b393bb97` + `6625a63e`: espelho de 1 bit por região.
   - A/B de `KYTY_READBACK_SIDE_COPY_WINDOW_KB=256/1024` (`2559c273`).
     - A nossa janela de 64 KiB com +32–44% é outra coisa: é o fault-ahead de escrita.
     - A de leitura tem padrão de 64 KiB e teto de 1 MiB.

## Prioridade 2 — correção (risco baixo, poucas linhas)

- **`8a188303`, índices de primitiva NGG com 9 bits, não 10** (`spirvEmitterMesh.cpp:258`, confirmado). Com edge flag ligada, o índice passa de 511 e o nosso guard descarta o triângulo, então some geometria. Mesa faz igual.
- **`89683130` + parte de `511447c7`, PRIM_AMP_FACTOR no GS meshlet.** `max_primitives` e `threads_num` cobrem só os vértices (`shader.cpp:1383-1388`), então primitivas a mais são cortadas.
- **`c5d0f907`, leitura escalar além de `num_records` no walk de SRT dá 0** em vez de falhar e pular o draw (`SrtWalker.cpp:1342,1376`).
- **`c47f0bb1`, marcador binary-info só dentro do código.** Um shader que começa com `s_mov_b32 vcc_hi, imm` faz `GetBinaryInfo` ler muito além do código. O commit tem só o teste; o fix teria de ser escrito.
- **`28b1e6c8`, omod do VOP3 zera denormais e -0.** Aplica limpo e muda a versão do codegen.
- **`3e8c01a0`, `UploadCopies` sem alias de backing** faz memcpy direto do endereço do guest e dá SIGSEGV em memória desmapeada. Usar `TryReadBacking` e, se falhar, zerar.
- **`2e002fb5`:** não capturar snapshot de loads escalares de bases que o próprio shader escreve (`SrtWalker.cpp:360`).
- **`57f1c6c5`:** manter imagem ou buffer liberado até a GPU passar do último tick que o usou. Hoje apagamos no fim do tick do `Delete*`, e o draw-prep assíncrono pode usar o recurso num tick depois. Rede barata contra DeviceLost.
- **`3861fd94` (só a guarda):** os coletores liberam imagem modificada pela GPU que não é `SafeToDownload` e perdem o conteúdo (`textureCache.cpp` ~6195/~6260). O fix é um `continue`.
- **`48028863` e `3cd1568d`:** casos de pressão de memória.
  - `48028863`: o write-back pendente da imagem despejada ainda não terminou quando a imagem é liberada.
  - `3cd1568d`: o fallback de buffer para memória do sistema, e não sair com `EXIT_IF` no GC.
- **`d898d759` e `5ebc89da` (risco médio):**
  - `d898d759`: fetch embutido só de constantes não deve ganhar atributo nem buffer.
  - `5ebc89da`: palavra de descritor que vira teia de Phi faz o tracking desistir. Só vale se aparecer a desistência no log.
- **Libs dentro de `d6a807a4`:**
  - os diálogos de save e de mensagem em modo barra de progresso devem ficar RUNNING (`dialog.cpp:464-476`);
  - aceitar pasta de save antiga sem `sce_sys`.
- **`f91035a9`** (`sceAcmBatchJobPriority` como no-op) e **Opus família 1 de `4e5f683a`**: aplicam limpo.

## Prioridade 3 — testar antes (A/B ou checar se o jogo usa)

- **Áudio (`93a39be0`, `9151c576`, `a64178a8`), risco médio.**
  - `audioout2_queue_context_audio` segura `g_audioout2_port_mutex` durante um `AudioOutOutputs` que bloqueia (`libAudio2.cpp:462-488`), e o nível da fila não vem do dispositivo.
  - No fork, isso dava áudio a 60% do tempo real, estalos e música acelerada.
  - Primeiro ver quais dos nossos jogos usam `sceAudioOut2ContextPush`.
  - Ajustar a folga para PipeWire e manter um interruptor de escape.
- **`171338e7`:** barreira de acquire em loads coerentes (GLC). O fork ligou isso a meia tela velha na RX 9070 XT, no Windows. No RADV o `Coherent` talvez já baste, então fica opt-in com A/B no Crash e no Yotei.
- **`0c6aad6d`:** interruptor para o guard de clip de posição zero, que sempre emitimos (`spirvEmitterModule.cpp:795`). O clipper da AMD talvez já descarte esses primitivos. A/B.
- **`633e0096` e `dcac2bb4`:** contagens de dispatch e draw indireto lidas na GPU, em vez de a CPU esperar. Ver antes `DrawIndirectInstanceReads` e quantos dispatches com dimensão de thread temos sobre argumentos escritos pela GPU.
- **`c26ad60c` (parte):** ignoramos o DrawIndex do `DRAW_INDIRECT_MULTI` (bit 31 do dword 3, `pm4Handlers.cpp:1716-1740`). Logar se os nossos jogos usam.
- **`6f0c0ef6`:** lista de shaders a pular, como diagnóstico opt-in. Útil para o hang de RT do Astro.
- **`4ebe2db5`, `999aa639`, `eb3f9128`:** pipelines pequenos, UI e alvos pequenos nunca pulados. Só importa se ligarmos `KYTY_ASYNC_PIPELINES`.
- **Tempo de compilação:**
  - `b2a56eb9`: dominadores em ordem DFS.
  - `c7344739`: acessores de IR inline.
  - `38e3d08c`: `MemoryIndexBelongsTo` é quadrático.

## Já temos ou não serve

- **Já temos:**
  - os quatro fixes específicos de AMD (branches escalares wave64 decididos pela wave inteira, subgroup size exigido, wrap do V_MBCNT, clock do S_MEMREALTIME);
  - gravação em thread, cache de pipeline sem revisão, cache de shader em disco, estado dinâmico;
  - revalidação de T# do bindless, samplers do jogo, volumes BC no RADV;
  - janela de readback, cache de mapeamentos por thread, ISSUES #26.
- **N/A:**
  - tudo de fast-launch: o merge que fizemos deixou o 11001c97 de fora, e `fast_launch` não aparece em `src/`;
  - os limites de mesh e o cull da NVIDIA;
  - Windows (`129e9bf1`).
- **Pular:**
  - o walker de SRT nativo e a série de replay: no Crash 4 o walk já é barato. Reconsiderar `598030de` só se a preparação de dispatches (item 2) for descartada;
  - teto de cache de 6 GiB, núcleos híbridos, uploads na thread de gravação, walks VS/PS em paralelo;
  - `0acca608` (ordem de LDS para o escalonador da NVIDIA);
  - import/export do launcher, que conflita com os nossos perfis.

## Pontos abertos

- Confirmar se `ResyncGpuRangeProtection` (`memory.cpp:114-133`) rearma o write-protect do UFFD quando o write watch está ligado (ligado a `17c31c72`).
- O `d6a807a4` deixa o recompilador pular um shader que ele não consegue tratar, em vez de encerrar. Fica como decisão de política para o dono do recompilador.
