# O que aproveitar do fork chenxiao07/KytyPS5 na guest-sync-release-mem

Data: 2026-10-07. Base de comparação: `guest-sync-release-mem` em `58bc3172`, que já inclui o lote A do int17 e o B1/B2 do Codex. Nada deste documento foi portado ainda.

**Regra do usuário:** as nossas otimizações ficam. Nada pode substituir ou reverter:

- a sincronização BDA;
- o CP sequencer e o recorder;
- os certificados do DrawPrep e os draw runs;
- a velocidade de compilação de shader;
- os loads de storage buffer sem branch;
- os strides em runtime e os bindless;
- a chave do program cache.

## 1. O fork

- **Repositório:** `https://github.com/chenxiao07/KytyPS5`, branch padrão `experiment/perf-40fps-20260926`, topo `4f7bacb4` (2026-10-08).
- **No repositório local:** os branches estão em `refs/chenxiao/perf-40fps`, `refs/chenxiao/ai-optimization-skill` e `refs/chenxiao/demons-souls`.
- **Foco do fork:** Demon's Souls (PPSA01341), Windows, Intel i9 com NVIDIA RTX 5090. O próprio README avisa que outros jogos e GPUs AMD não foram testados.
- **Tamanho da divergência:**
  - a base comum com a nossa branch é `2e315a3c` (2026-09-10);
  - desde então ele tem 278 commits e a nossa branch tem mais de 2000;
  - 273 commits dele não existem na nossa branch (comparação por patch-id);
  - simulando o cherry-pick de cada um sobre o lote A, só 9 entram limpos e 264 conflitam.
- **Natureza do trabalho:** a maior parte dos 126 commits `perf` depende de subsistemas próprios do fork, como os draw records "native XPR", o "table mode", os planos SRT compilados AOT numa DLL e o worker de gravação Vulkan com submissão adiada. Esses subsistemas concorrem com o nosso CP recorder, o DrawPrep e as receitas de SRT.

**Como foi verificado:**

- `git merge-tree` simulou cada cherry-pick sobre `port-int17-lote-a`;
- três revisões separadas cobriram as correções gráficas, as de kernel/memória/áudio e os commits de desempenho;
- cada item foi conferido de novo no `HEAD` integrado.

## 2. Já temos (não portar)

| chenxiao | O que é | Onde já está |
| --- | --- | --- |
| `6e7c1dab` | Fibers em pilha própria | lote A `c68fc25c` + `e44f56aa` |
| `3f382515`, `e6dfd8e6` (parte) | Proteção em memória remapeada | lote A `785ab1d0` |
| `3248e372` | Ids de arquivo do APR, um por caminho | lote A `cd18c9cb` |
| `86910c0d` | Imagem na RAM do sistema sem VRAM | lote A `47d1138c` + `9c3ce1af` |
| `05e64602` | `KYTY_VRAM_LIMIT_MB` | lote A `6edb6768` |
| `150517a8` | TerminateProcess no erro fatal (Windows) | lote A `08a3c281` |
| `397112a0`, `e6b7fb0b` | Storage repetido sem barreira, fill largo como clear | lote A `07325370`, `4ef0c607` (desligados por padrão; ver 3.3) |
| `3b351932` | Start instance do draw indireto na SGPR (recomendado pelo autor) | B1 do Codex (`ffa2bcd9`), com o bug de mesh corrigido |
| `f203d0f3` (áudio) + `c2b48aeb` | Objetos 3D do AudioOut2, ambisonics e downmix 7.1 (recomendados pelo autor) | B1 do Codex (`d09339da` do Jetsku, que é baseado nesses dois e é melhor para nós: SDL3, PCM copiado, `pcm_fresh`, pan por posição, testes) |
| `f203d0f3` (ATRAC9) | RIFF exigia o chunk `data` inteiro | `atrac9_decoder.h:481`, vindo do upstream (`e40438f9`/`92ae218d`) |
| `9f912da0`, `b200a57b` | Tabela nula lê zero; dispatch cujas tabelas não avaliam é pulado | `c6a98f60`, `f8c87b86`, `7638f642`; `pipelineCache.cpp` (descarte do estágio) |
| `a28de66f` (validação) | Sem validação de IR em release | `ValidateTranslatedIr()` / `ValidateEmittedIr()` |
| `643d6391`, `2780eb24` | TLS da fiber e capacidade do save; clobber de RBX | `FiberGetThreadContext`, `44bf0207`; `pthread.cpp:946` |
| `abb97b22`, `bf6a3a8b` | Imagem escrita pela CPU deixa de ser da GPU | `b386b88a` (ReleaseCpuOverwrittenImages) |
| `098acd88`, `08b97167`, `4a4faf72`, `c16fdb88` | Descritores inválidos de vários tipos ligados como null | `baa4b080`, caminho de footprint inválido, layout do formato 129, `ValidImageDescriptor` |
| `e80c4bc6`, `8f09f3a2`, `cb6c9592`, `f38424a0`, `b5cd9f95`, `05677f75`, `f55d56bc`, `5744bf31`, `befaeb04` | Vários (metadados de hash, indirect, stencil, layout de depth, stores tipados, branches de wave, material key) | Equivalentes próprios (`KYTY_SHADER_METADATA_BACKING`, `ReadGuestForCp`, `af8d7f40`, `40966f43`/`aeabb5f0`, `259bb8b8`, `d9b88645`, `098990bf`) |
| `66708526`, `96a728a5`, `839a57d2` (busca) | Tick sem o lock da fila; contagem por thread; busca de faixa livre | Nossos modos queued/recorder; `CountFrameEvent` por thread; lote A `1b1c917d` |

## 3. Vale portar, sem mexer nas nossas otimizações

### 3.1 Correções gráficas (tiram EXIT/crash ou corrigem renderização)

| chenxiao | O que corrige | Como portar | Risco |
| --- | --- | --- | --- |
| `ec9bd1e2` + `5cb588f9` + `0a1f06d9` + metade de storage do `b310b97c` | Texturas que o Vulkan não representa, T# inválido, T# fora da memória do guest e storage view inválida: hoje viram EXIT (`tile.cpp` `TileGetTextureTotalSize` com `EXIT_NOT_IMPLEMENTED`; `descriptors.cpp` em `BuildTextureDescription`/`ValidateStorageTexture`; `image.cpp:205`). Passam a ser ligadas como null. | Adaptar: `TileGetTextureTotalSize` devolve bool, e `BuildTextureDescription` usa `NullTextureDesc`. Só roda em miss do cache de descrições. | Baixo |
| `a890d1f5` | `ImageSubresources` compara de forma lexicográfica (`<=>`, `imageInfo.h:39`). Uma imagem com mais níveis e menos camadas pode ser reaproveitada errado (`textureCache.cpp` ~3885, 1889, 1938, 2057). | Trocar por `Contains()`. | Baixo (observar o churn de imagens) |
| `0fca2ede` (só a parte do SRT) | O `SrtWalker` tem `SGreaterThanEqual32`, mas não `UGreaterThanEqual32` nem `Identity`, então SRTs com `s_cmp_ge_u32` não avaliam. | Acrescentar os dois opcodes. O resto do commit (offsets de bytes, LOD) já está coberto ou mexe nos loads sem branch, então fica de fora. | Baixo |
| `e22b79c8` | Fills de padrão em dword reconhecidos como clear | Entra **limpo**. Pré-requisito do HTILE (3.4). | Baixo |
| `89be1951` | Imagem cuja origem cai em páginas não mapeadas dá EXIT no staging (`bufferCache.cpp` ~3855) | Corrigir só na chamada `ObtainBufferForImage`. **Não** mexer no `SynchronizeBuffer`, que é o caminho quente do BDA. | Baixo |
| `f7f4dd65` | `RebindImages` faz uma passada só e não vê o `depth_id`; o `FindTexture` dá EXIT | Repetir até as imagens estabilizarem, com cuidado com o memo e os view runs. | Médio-baixo |
| `601d184b` (só DEFAULT_VAL) | PS lê entradas indefinidas quando o SPI pede valor padrão (`USE_DEFAULT`, `DEFAULT_VAL`) | **Conferir antes:** já existe algum `DEFAULT_VAL` no codegen (`CodegenOptions.cpp`, `spirvEmitterInternal.h`). | Baixo |
| `74410dbc` | Nomeia os shader modules pelo hash do guest | Só debug; trivial. | Nenhum |

### 3.2 Correções de kernel, memória e IO

| chenxiao | O que corrige | Como portar | Risco |
| --- | --- | --- | --- |
| `ee87351b` | Leituras de arquivo direto na memória do guest (`fileSystem.cpp:643-646`, `781-788`, `891-893`). Uma página protegida no meio dá EFAULT ou leitura curta, e o `ferror` é ignorado (`sysLinuxFileIO.cpp:90`). | Manter a leitura direta. Só numa leitura curta que não seja EOF, usar o buffer intermediário e registrar o `ferror`. Assim o carregamento normal não ganha `memcpy` a mais. | Baixo |
| `130c08d9` | `TryReadPrtBacking` (`memory.cpp:1553`) lê sem lock enquanto Map/Pool alteram as faixas virtuais. Uma leitura que falha termina em EXIT (`bufferCache.cpp:3856`). | Mutex de residência, mais o motivo da falha. São uns 12 blocos no `memory.cpp`. | Médio-baixo |
| `bb0fbe7e` | Um `int 0x41` (debug break do jogo) sem debugger mata o processo. No Linux ele chega como SIGSEGV em 0 e vira EXIT (`hostException.cpp:275-283`). | Disparar pelos bytes `cd 41` no RIP dentro de um módulo do guest. O `TrySkipDebugBreak` já tem o ramo de ucontext. | Baixo |
| `c680f054` + `90c7c215` | `WriteBacking` dá EXIT ao escrever em memória desmapeada ou privada (`memory.cpp:1498`) | Opcional: o nosso `UnmapMemory` já espera a GPU antes de desmapear. Vale como blindagem. | Baixo |
| `0d7c2140` | Erro fatal sem backtrace no Linux | Entra **limpo**. Chamar `backtrace()` uma vez no início, para a primeira chamada não carregar a libgcc durante o crash. | Nenhum |
| `eed800b1` (+ `3baa8490` do Jetsku) | A linha de EXIT não diz módulo + offset; não há dump da memória para onde os registradores apontam | Pequeno. O `3baa8490` (`faultMemoryDump.h`, com testes) não entrou no B1 e pode vir junto. | Nenhum |

### 3.3 Desempenho sem conflito com a nossa arquitetura

| Item | O que faz | Observação |
| --- | --- | --- |
| **Testar os switches que já temos:** `KYTY_GUEST_STORAGE_REPEAT=1`, `KYTY_WIDE_FILL_CLEAR=1`, `KYTY_AMD_BUFFER_BOUNDS=1` | Os dois primeiros são os `397112a0`/`e6b7fb0b` do chenxiao, já no lote A. O terceiro é a nossa base do `357b3ac4`. | Nada a portar, só A/B. No Demon's Souls, o primeiro sozinho subiu de 33.9 para 40.6 fps. Reduzem tempo de GPU, que é do que dependem os "CPU waiting on GPU". Conferir screenshots. |
| `59aa6dc7` | Quem chama `SendCommandSync` espera girando ~20 µs antes de dormir no futex (o nosso usa `binary_semaphore`) | Pequeno; manter o escopo do HangWatchdog. |
| `8a816204` | Memo do shader map de 1024×2 entradas (o nosso tem 16) | Conferir antes o contador `ShaderMapMemoMisses`. |
| `5a12d808` | Dedup das leituras de SRT por bucket de hash (o nosso compara cada leitura com todas as anteriores, `SrtWalker.cpp:426`) | Menos travadas no primeiro uso. Conferir SPIR-V idêntico com `--compile-benchmark`. |
| `02041c81` | Itens do LRU em blocos fixos em vez de `std::deque` | Entra **limpo**. Ganho pequeno na libstdc++ (o problema era maior no MSVC). |

## 4. Vale, mas mexe em caminho otimizado (um por vez, com medição A/B)

| chenxiao | O que faz | O que tocaria | Risco |
| --- | --- | --- | --- |
| `54470a7e` + `bac2820d` (depois do `e22b79c8`) | Usa o valor do fill do HTILE para decidir o clear de depth e stencil. Hoje qualquer escrita no endereço do HTILE vira clear total de depth (`renderCompute.cpp:77-87`, DMA em `bufferCache.cpp:3878`, aquisição em `renderDraw.cpp:1006-1016`), e o stencil nunca é limpo. | Registro de meta por slice | Médio |
| `357b3ac4` (sobre `KYTY_AMD_BUFFER_BOUNDS`) | Sem guarda de EXEC em loads de storage cujo limite o device já checa; condição constante gera código reto. No Demon's Souls, o shader de pós-processo caiu pela metade (~+6% fps). | **O nosso emitter de loads sem branch.** Muda o SPIR-V. BDA, atomics e subdword precisam manter as checagens. | Médio |
| `29b1c36d` | O present não segura o lock das portas do videoout. Hoje o `FlipQueue::Flip` segura todas as portas do grupo durante o `Present`, e o Thread_Gpu trava no `Prepare`/`GetFlipStatus`. No Demon's Souls, o 1% low subiu de 42.8 para 46.3. | `FlipQueue`; checar de novo opened/closing/generation depois | Médio-baixo |
| `bcc715d1` | Escrita do guest em memória da GPU usa readback assíncrono. Hoje o `side_path` exige `!is_write` e cai no dreno síncrono (`SendCommandSync` → `ReadMemoryDrain`); no Yotei são 356 esperas/s e 258 ms/s. | Os nossos side readbacks; ordem de posse e publicação | Médio |
| `bb82105c` (só a parte das cópias em sequência) | Cópias de buffer seguidas compartilham uma barreira antes e uma depois. No Demon's Souls os drenos de GPU caíram de 265 para poucos por frame. | Batcher de barreiras; uma cópia que se sobrepõe abre uma sequência nova | Médio |
| `58e2fd8b` | Escritas do Thread_Gpu em páginas protegidas vão pela view de backing, mantendo a proteção. Mira no custo de fault e mprotect do Crash 4 (176 faults/frame × 119 µs). | Dirty log e sincronização incremental do BDA | Médio-alto |
| `4a979df3` | "Upload prologue": cópias de páginas que ficaram sujas desde o início do command buffer vão num command buffer submetido antes. No Demon's Souls, os render passes caíram de 197 para 107 por frame. | CP recorder, broker e certificados do DrawPrep | Alto (medir antes os flushes causados por upload) |

## 5. Não portar

| Grupo | Commits (exemplos) | Motivo |
| --- | --- | --- |
| Native XPR, table mode, SRT AOT/DLL | `7eec0198` e os ~15 `native-xpr`, `5c5c8cef` e os ~8 `table`, `adee1fd8`, `a0c12e16`, `1952ecc4` | Concorrem com o CP recorder, o DrawPrep e as receitas e tapes de SRT. |
| Worker de gravação Vulkan / submissão adiada / fila de readback | `56184e3e`, `38769579`, `6aee2377`, `0de8dbc1`, `17158293`, `b02d6cb5`, `9298d5ee`, `5a09979e` | Concorrem com o nosso submission mode queued e os side readbacks. |
| Preparação e compartilhamento de draws | `6684d25f`, `4af8b5ec`, `06281a35`, `347a9116`, `4065f7f4`, `18085988`, `79b98420`, `93ba7a73`, `da133596`, `9cda59b7` | Duplicam o DrawPrep, os draw runs, o binding memo e o hash de pipeline. |
| Sincronização BDA por região | `fe7e3251`, `4a1ba8c4`, `fba4c3d7`, `9bffcaeb`, `a4b3e728`, `3d0d432f`, `a8cec954`, `e5d883c3` | Feitos sobre o gpuResourceManager do fork; a nossa sincronização BDA cobre isso. A ideia do `a4b3e728` é parecida com a sincronização por submissão que já testamos e descartamos (~2%). |
| Kernel e memória (perf) | `45ca75ed`, `ef9228eb`, `acbde255`, `435a9f89`, `18e290f7`, `476ae208`, `a678f274` | Já superados ou cobertos (`a52d9e84`, `ClampRangeMemo`, backing reads, fault-ahead, `KYTY_UPLOAD_DMA_HOST_COPY`). |
| Pipelines e precompile | `3d156102`, `587fe911`, `5b7fb890`, `c28cd752`, `393ecdde`, `94b49a19`, `4163cfe1`, `d389e01d`, `704e4fb7` | Fast-first, program cache, precompile e prefetch já cobrem; os pipeline binaries são do fluxo do fork. |
| Tabelas de textura waterfall e indiretas | `2fead008`, `232f38fc`, `e85a81c9`, `fbd44f9f` | Dependem de uma passada que não temos; o nosso tracker e o bindless cobrem; brigaria com o nosso trabalho de SRT. |
| Específicos do Demon's Souls | `749a2bcd`, `545a1aad`, `41eb32db`, `0ac332e0`, `5a219b30`, `455f8d6b`, `dc02d902`, `8791b592`, `7dfd79fc`, `a7d46e82`, `84b11a65` | Hooks, warp e patches do jogo. |
| Windows, launcher.cmd, empacotamento, `src/local`, `tools/*.ps1` | `43f1271e`, `400d414b`, `c848b5a7`, `2fe2c1b5`, `93066a97`, `4f7bacb4`, `03e3730c`, `f02d354d`, `b257bc37`, `678fd668`, `6f4e6a12`, `66092953`, `4deac11f` | Só Windows ou só do fluxo de teste do fork. |
| Outros | `81cd3118` (GC com menos de 32 GB), `e5738def`, `ce7b6a3f` (resto), `e361ee81` (resto), `f0aa51ba`, `9d1fabe2` | O nosso GC é diferente; não se aplica; ou depende de algo que não temos. |

## 6. Ordem sugerida

1. **A/B dos switches que já temos** (3.3): `KYTY_GUEST_STORAGE_REPEAT`, `KYTY_WIDE_FILL_CLEAR`, `KYTY_AMD_BUFFER_BOUNDS`. Não precisa de código.
2. **Correções de baixo risco:** primeiro 3.1 e 3.2 (null em vez de EXIT, `Contains`, opcodes do SRT, `e22b79c8`, leitura de arquivo, PRT, `int 0x41`, backtrace), depois os pequenos de 3.3. Cada um com teste e build limpo.
3. **Seção 4, um por vez:** `29b1c36d` primeiro (o mais isolado), depois o HTILE, o `357b3ac4`, o `bcc715d1` e o `bb82105c`. O `58e2fd8b` e o `4a979df3` só depois de medir.
