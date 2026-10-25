# Plano de gargalos: Wolverine no RX 9070 XT (2026-10-22)

Escrito pelas duas sessões que trabalham nesta árvore: `wolverineport-86` (lado da GPU e da
compilação) e `wolverineport-02` (readback das threads do jogo, RAM e VRAM). Cada sessão edita só a
própria seção. A tabela de gargalos é comum: quem medir algo novo acrescenta uma linha.

## Regras
- O usuário não faz mais rodadas de teste. Medimos só com as partidas normais dele: o `Play.bat`
  guarda cada uma em `fase1\rodadas\AAAAMMDD-HHMMSS` (log, `fps.csv`, perfis aos 60 s e 150 s).
- Nada de abrir o emulador, nem em rodada automática.
- Toda otimização nova fica atrás de um switch `KYTY_*`. No build do teste final entram ligadas,
  cada uma com `=0` para desligar. O exe das 19:36 fica como reserva (`kyty_emulator.1936.exe`).
- Mudanças de risco (memória, sincronização, readback) não viram padrão sem uma partida normal que
  mostre que não quebraram.
- Premissas do Vulkan conferidas na especificação (Context7: `/khronosgroup/vulkan-docs`,
  `/gpuopen-librariesandsdks/vulkanmemoryallocator`).

## Gargalos medidos

| # | Onde | Custo medido | Origem do dado | Dono |
|---|------|--------------|----------------|------|
| 1 | Threads do jogo esperando readback (`HandleFault` → `ReadMemory`) | 53 % da thread principal: fence da cópia 27, `MasterSemaphore::Wait` 14, `SendCommandSync` 11; ~500 leituras/s | `lift-prof_150` | 02 |
| 2 | GPU thread: `TryClampRangeSize` reconstrói os intervalos mapeados (`RebuildSpansUnlocked`, percorre o `std::map` inteiro) a cada mudança de mapeamento | 14,2 % da GPU thread (284 de 1998 amostras) | `evict-prof_150`, endereços 0x140caf790-7dc | 86 |
| 3 | GPU thread: `VirtualProtect` (`PageManager::ResyncHostProtection` / `UpdateRegionWatchers`) | 5,6 % (19:06) a 10 % (controle) | `evict-prof_150`, `controle-prof_150` | 86 |
| 4 | GPU thread: cópias da memória do guest (`TryTransferBacking`, `TryReadSparseBacking`, `BufferCache::CopyBuffer`, memmove/memcpy do VCRUNTIME) | ~7 % | `evict-prof_150` | 86 (cópia), 02 (quando é readback) |
| 5 | GPU thread: preparo de bindings (`CommitBindings` 3,9 %, `FindBuffer` 3,3-4,1 %, com 42 % das chamadas em `unordered_map`, `TouchImage` 1,9 %, `HasGpuDirtyBytes` 1,8 %) | ~11 % | `evict-prof_150` | 86 |
| 6 | GPU thread: heap (`RtlAllocateHeap`/`RtlFreeHeap`, `~vector` 1,4 %) | ~4 % | `evict-prof_150` | 86 |
| 7 | VRAM cheia aos ~4 min (texturas bindless presas para sempre) | queda por falta de memória aos 264 s | `vram-oom-kyty_log.txt` | 02 (`KYTY_BINDLESS_EVICT`, já padrão) |
| 8 | Ida para a RAM quando a VRAM acaba falha com memória livre | o buffer de 5 MB não coube nem na RAM | `vram-oom-kyty_log.txt` | 02 |
| 9 | Pop-in: orçamento de texturas bindless (128 por rodada, até 4 rodadas por quadro); ou draws pulados enquanto o pipeline compila | sem medida (contadores estavam desligados) | `descriptors.cpp` ~1987 | 02 (bindless), 86 (pipelines) |
| 11 | Travadas de streaming: lotes de mapeamento do AMM esperando o marcador da leitura de arquivo (APR) | 96 esperas = 7,3 s em ~5,5 min; média 76 ms, máximo 267 ms, 14 acima de 100 ms. Parte da GPU no unmap: 0,1 ms | `evict-kyty_log.txt` (`[amm-wait]`, `[amm-batch]`) | 02 |
| 12 | RAM do processo | sem medida: o `LogCommit` é LOGF. Na queda por VRAM, o heap de RAM do driver estava em 730 MB de 15,5 GB, e o sistema tinha 21 GB livres | `vram-oom-kyty_log.txt`, consulta ao Windows | 02 |
| 10 | Blocos amarelos no vidro (só na AMD?) | — | capturas 16:01 e 18:11 | 86 (`KYTY_WAVE_LDS_HOST_SUBGROUP`, desligado, sem veredito) |
| 13 | Partida com cache frio (versão do cache mudou no build 20:01): pipeline compute `dec083415ec94cc6` compilando 30+ s na GPU thread; `_ShaderDump/slow_pipelines.txt` nunca era gravado (pasta inexistente) | média 15,0 → 10,3 fps, 28 s abaixo de 5 fps; 78 acertos e 179 compilações (partidas quentes: 95-99 % de acertos) | `kyty_log.txt` de 22/10 20:26, `prof_150` | SpartanPort (feito, build 23/10 00:27) |
| 15 | Congelamentos de 3-7 s no gameplay: a GPU thread espera o driver compilar, otimizado, o pipeline de um draw que não pode ser pulado (escreve memória, pequeno ou de interface; esperas de até 10 s / 3 s) | 42 % da GPU thread em `TryGetGraphicsPipeline` → `PipelineCompiler::Wait` no perfil dos 60 s; 4 compiladores no `amdvlk64` ~50 % do tempo (fila vazia no resto); 102 pipelines novos, 199 shaders do guest novos | `fase1\kyty_log.txt`, `prof_60` de 24/10 00:24 | 0c (feito, build 24/10) |
| 16 | Leituras do jogo presas ~2 s na fila da GPU thread durante os congelamentos que sobraram: os comandos só são atendidos entre pacotes PM4, e a GPU thread está dentro de um draw esperando o pipeline | 19 leituras > 500 ms, 9 > 1500 ms, quase todas `[queue ~1991-2119 ms]`; `prof_150`: GPU thread 57 % em `TryGetGraphicsPipeline`, thread do jogo em `SendCommandSync` | `fase1\kyty_log.txt`, `prof_150` de 24/10 00:53 | 0c (`KYTY_PIPELINE_WAIT_SUSPEND` + `KYTY_COMPUTE_ASYNC`, opt-in no `Play.bat`, build 24/10) |

## wolverineport-86: GPU thread e compilação
- **#2, intervalos mapeados (feito, build 19:50, não instalado):** `KYTY_CLAMP_SPANS_LOCAL` (padrão
  ligado; `=0` como antes). Cada mudança de cobertura (`Add`, `Remove`, `ReleaseReserved`,
  `ReplaceSpan`, que só mexem nos bytes de `[start, start+size)`) anota a sua janela. Uma consulta
  usa a lista antiga se nenhuma janela toca o span dela (bordas incluídas, porque uma mudança encostada
  pode juntar ou partir o span); se alguma tocar, ou se passarem de 64 janelas, reconstrói como hoje.
  A resposta continua exata, não é aproximação. `src/kernel/coverageSpans.h`, com o teste
  `tests/CoverageSpansTests.cpp` (400 mil passos aleatórios comparados com a reconstrução completa;
  falha numa versão sabotada que ignora as bordas). `KYTY_CLAMP_SPANS_VERIFY=1` confere cada resposta
  no jogo e fecha o emulador mostrando as duas, se diferirem.
- **#3, `VirtualProtect`:** feito em parte, `KYTY_SKIP_VIEW_PROTECT` (padrão ligado; `=0` como
  antes; do upstream c30ac7c9). O `MapViewOfFile3` já mapeia a view com a proteção pedida, então o
  `VirtualProtect` logo depois só é necessário para `NoAccess`. Isso tira uma chamada de cada map do
  streaming. Teste `WindowsBackingViewPermissions` (VirtualQuery em cada modo). Falta juntar as
  proteções adjacentes do `PageManager` numa chamada só.
- **Trazido do GitHub (build 20:01, instalado; todos com switch e ligados por padrão):**
  - `KYTY_WAVE_LDS_FENCE`: barreira só de memória, de escopo subgrupo, onde o `WaveLdsScope` não
    achava escopo (compute de várias ondas que escreve LDS: na AMD, nenhuma ordem de LDS). Não trava,
    porque ninguém espera nela. Ideia do Jetsku d47cc5f9. Suspeito dos blocos amarelos.
  - `KYTY_FIRSTLANE_EXEC_ZERO`: `V_READFIRSTLANE` com EXEC zero lê a lane 0, como o hardware. Antes
    era indefinido no caminho nativo e dava a lane 31 no dividido (upstream 919fac13).
  - Tiler com seletores escalares (Jetsku 7d881089, mais os dois PRT que nenhum dos dois forks
    corrigiu): o compilador da AMD no Windows gera errado o `Select` vetorial de spec-constant. Mesma
    tabela de antes. Sem switch, porque o resultado é idêntico.
  - A versão do cache de shaders mudou (`0x372ebaba464a97e4`): a primeira partida recompila tudo.
  - Testes: `coverage_spans`, `memory_tracker`, `page_manager`, `shader_cfg`,
    `shader_vertex_metadata`, `srt_native` e `--lds-limit-only` passam. Duas falhas que já existiam
    (iguais com os switches desligados): `GuestStackUsesPrivateOwnerMemoryAndCache` e o
    `ShaderFunctionExpansionCache`. A segunda aborta o `--wave64-only` antes dos casos de wave64.
- **Próximos:** helper lanes fora dos laços de CAS em pixel shader (Jetsku afb40969, TDR na AMD:
  falta declarar `gl_HelperInvocation` no nosso emissor); `PageManager` com proteções juntas;
  `FindBuffer` sem o `unordered_map` repetido.
- **#5 e #6:** tirar alocações por draw (vetores temporários) e buscas repetidas no `unordered_map`.

## wolverineport-02: readback, RAM e VRAM

Feito, compilado (`_Build/port`, só os .obj) e ainda não instalado:
- **#1, readback:** `KYTY_READBACK_QUEUE_PRIORITY` (padrão ligado, `vulkanWindow.cpp`): a fila
  gráfica fica com prioridade 0,5 e a fila de readback com 1,0. Mira os 27 pontos de fence: as
  cópias de poucos KB rodam atrás do quadro no mesmo motor. A especificação permite que o driver dê
  mais tempo à fila de prioridade maior e não garante nada; a partida final mede pela fatia
  `MasterSemaphore`/fence do perfil automático. `=0` volta ao igual.
- **#7, VRAM:** `KYTY_BINDLESS_EVICT` saiu da tabela de padrões do Frank (`main.cpp`). Partida das
  19:06: 338 s sem queda, sem texturas erradas, sem device lost.
- **Medição em partida normal (só log, no `Play.bat`):** `KYTY_READBACK_STATS` e
  `KYTY_SHARED_MEMORY_PROBE` (páginas e buffers mais lidos, real contra falso compartilhamento),
  `KYTY_BINDLESS_STATS` (resolvidas e adiadas por orçamento, #9), `KYTY_APR_STATS` (novo, em
  `libAmpr.cpp`: leituras de asset divididas em invalidação, disco e cópia, #11) e
  `KYTY_MEMORY_STATS` (novo, em `vma.cpp`: working set, memória privada, commit do sistema e VRAM a
  cada 10 s, #12; `QueryProcessMemory` em `sysWindowsVirtual.cpp`).

Próximo, escolhido pelos números da primeira partida com esses logs:
- **#1:** se as páginas mais lidas caem em buffers pequenos, fazer os shared buffers (memória do guest
  importada, `VK_EXT_external_memory_host`) cobri-los; hoje só 10 entram por partida, e 8 deles
  são descartados. O VMA recomenda memória host-visible e com cache para leitura pela CPU, que é
  exatamente o que a importação dá. Se a maioria das falhas for falso compartilhamento, servir essas
  leituras sem cópia.
- **#11:** se o tempo for de invalidação, reduzir a disputa do `m_lock` do texture cache com a GPU
  thread; se for disco, ler o próximo pedaço enquanto o anterior é copiado.
- **#8:** a mensagem de falta de memória já mostra o resultado das três tentativas; corrigir pela causa
  que ela mostrar.
- **#9:** subir o orçamento bindless só se `budget=` crescer nas partidas.

## Sessão SpartanPort (2026-10-23): o que dá para trazer de lá

O SpartanPort (God of War III, PS4) roda o `eboot.bin` direto na CPU, troca as bibliotecas do
sistema por um runtime escrito só para esse jogo e usa o núcleo de vídeo do shadPS4. O
`docs/parallel_gpu.md` dele mede cada otimização com A/B. Comparado com esta árvore:

| Técnica do SpartanPort | Ganho medido lá | Aqui |
|---|---|---|
| Código x86-64 do jogo direto na CPU, runtime HLE | base do projeto | já é assim no Kyty (frames `[guest]` nos perfis) |
| Núcleo de vídeo do shadPS4 | — | não serve: é GPU do PS4 (GCN); trocar o backend PS5 seria reescrita |
| Desproteger janela de 64-256 KiB por falha de escrita | +22 % | já existe (`KYTY_FAULT_GRANULE_KB`); 64 KB deu +3-5 % e congelou carregamentos (20/10), segue desligado |
| Readback de janela de 512 KiB | — | já existe, maior (`KYTY_READBACK_WINDOW_KB`, 4 MiB) |
| Aquecimento de pipelines em segundo plano | — | não feito: as partidas quentes acertam 95-99 % no cache do driver (sonda + gravação periódica já existem). Só vale com um perfil quente que mostre custo nos acertos |
| Preparo de draws em workers | +11,5 % | já existe uma versão (`DrawPrepScanner`, `KYTY_STAGE_PREP_PARALLEL`) |
| Pipeline de draw em dois estágios | +19 % | maior ganho de lá, mas é refatoração grande e delicada; fora do escopo |
| `UpdateImage` sem lock quando a imagem está limpa | +2 % | teto parecido aqui (`TouchImage` 1,9 % da GPU thread); não feito |
| LTO + PGO | −4,8 % µs/draw | LTO já é usado (`_Build/port-lto`); o PGO precisaria de uma partida com build instrumentado (mais lento) |
| Staging mantido entre rajadas (512 MiB) | parte da queda de 60-170 ms para 40-50 ms | não medido aqui |

Feito (#13, build de 23/10 00:27, instalado; o anterior ficou como `kyty_emulator.2001.exe`):
- `shaders.cpp`: o `SlowWatch` cria `_ShaderDump` antes de gravar uma entrada **compute**. Graphics
  continua como antes (sem a pasta, não grava): uma compilação lenta de graphics roda no worker e
  não trava o jogo, e um pipeline listado perderia os draws em todas as partidas seguintes.
- Pipeline compute listado: primeiro pede a versão otimizada ao cache do driver
  (`FAIL_ON_PIPELINE_COMPILE_REQUIRED`); só compila sem otimização se o cache não tiver. Assim a
  partida quente não perde o código otimizado. `KYTY_SLOW_PIPELINES=0` desliga tudo.
- Sem mudança na versão do cache de shaders (`0x372ebaba464a97e4`): `shaders.cpp` não entra no hash.
- A confirmar na próxima partida com cache frio: `[slow-pipeline] C dec083415ec94cc6 ... listed`
  na abertura, e a GPU thread sem a parada de 30 s. Na quente: `taken optimized from the driver
  cache` no log com `LOGF` ligado.

Partida de 23/10 00:30 (build 00:27): a lista funcionou (`_ShaderDump/slow_pipelines.txt` criado);
a parada desse compute foi de ~92 s (61→155 s), não 30 s. Depois dela, 26-27 fps até ~240 s; quedas a
0 em 285-296 s e 307-320 s; caiu aos 322 s num ponteiro lixo do próprio jogo (`Job Hi 0`, `mov
[rax+0xc]`, rax=`b0d0b80346005901`), mesma classe dos crashes de bytes velhos. O código novo não
rodou nessa partida (a lista foi lida antes da entrada existir).

**#14, cache do driver deixou de servir (aberto, maior gargalo medido agora):** desde o build 20:01
(versão `372ebaba`) as partidas quentes acertam ~9 % (24 acertos, 233 compilações; antes 95-99 %).
Não é o tamanho (270 MB acertava) nem o cache de tradução (a partida reescreveu 56 entradas, a fria
1142). O diff do emissor das 20:01 não tem nada que varie entre partidas. `KYTY_PIPELINE_MISS_LOG=1`
(só log, ligado no `Play.bat`, build 00:41) imprime `[pipeline-miss]` com hash do guest e do SPIR-V
de cada estágio e do estado estático: com duas partidas, o mesmo shader do guest com outro SPIR-V
prova que o código emitido muda por partida; SPIR-V igual aponta estado ou driver.

Perfis com símbolos: o PDB do build LTO só tem as funções públicas do C++ (por isso o `[freeze]`
mostra `boost::...::unlock`). O map do lld tem todas, então agora ele vai para
`fase1\kyty_emulator.map` junto com o exe. `..\profsym.py <map> <prof_N.csv> [tid]` dá o tempo por
função e por thread; `..\profcallers.py <map> <prof> <tid> <função>` dá as cadeias de chamada. O map
precisa ser do mesmo build do exe da partida.

## wolverineport-0c (2026-10-24): congelamentos por compilação (#15)

Partida de 24/10 00:24 (109 s): o contador de quadros fica parado de 3 a 7 s várias vezes no
gameplay (61-66, 73-78, 94-98, 103-109 s). O perfil aos 60 s caiu num deles (ver #15). Os acertos no
cache do driver voltaram (pelo menos 257 contra 102 compilações), então o #14 não se repetiu nessa
partida: os pipelines que faltam são de conteúdo novo de verdade. Nenhuma linha `cond-ib:` (o
Wolverine não usa o `COND_INDIRECT_BUFFER`).

Feito (`pipelineCache.cpp`, `shaders.cpp`, `pipelineCache.h`; nenhum entra na versão do cache de
shaders), cada um ligado por padrão, `=0` como antes:
- `KYTY_URGENT_FAST_COMPILE`: draw que espera o pipeline e o cache do driver não tem → compila sem
  otimização (`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`, como já se fazia para os compute
  lentos); a versão otimizada vai para um worker só para encher o cache (`PipelineCompile::CacheWarm`,
  fora da lista de lentos), e a próxima partida a pega pela sonda. Sem troca de pipeline no meio do jogo.
- `KYTY_COMPUTE_FAST_COMPILE`: o mesmo para todo compute novo (antes a sonda era só para os da lista
  de lentos). Os da lista não ganham aquecimento: o otimizado deles levou minutos e seguraria a saída.
- `KYTY_PIPELINE_URGENT_FIRST`: pipeline que a GPU thread espera vai para a frente da fila.
- Aquecimentos rodam só com a fila vazia e um por vez, para os outros workers ficarem livres.

Custo: na primeira visita, esses pipelines rodam sem otimização na GPU (a GPU tem folga nesta
máquina). A medir na próxima partida: segundos com o contador parado, e se a segunda visita à mesma
área continua sem congelar.

Build 24/10 00:51 instalado em `fase1` (anterior: `kyty_emulator.0022.exe`). Inclui o refactor da
sessão do Codex de 24/10 (SlotVector com índice denso, Singleton, `Common::Environment::IsSet` em 26
arquivos), menos os 6 arquivos do recompilador: lá a troca de nome mudava a versão do cache de
shaders (0x372ebaba → 0x5dfb4b25) e forçava retradução de tudo, então eles voltaram ao `getenv` (a
pedido do usuário; cópia do Codex no scratchpad da sessão 0c). Não trocar nome de nada nos arquivos
do recompilador sem querer invalidar o cache.

Partida de 24/10 00:53 com esse build (248 s de gameplay): média de 15,6 fps contra 8,9 antes; os
segundos abaixo de 10 fps caíram de 46 % para 19 % do gameplay. Ainda congela em 90-98, 125-130,
154-166, 176-178, 198-199, 230-233 e 274-279 s. O `prof_150` caiu no de 154-166 s: a GPU thread
57 % em `_Cnd_timedwait` ← `TryGetGraphicsPipeline` ← `DrawAuto`; uma thread do jogo em
`HandleFault` → `ReadMemory` → `SendCommandSync`. Das leituras do jogo, 19 passaram de 500 ms e 9
de 1500 ms, quase todas `[queue ~1991-2119 ms]`: o pedido ficou na fila da GPU thread enquanto ela
esperava um pipeline dentro de um draw (os comandos só são atendidos entre pacotes PM4). Em área
aberta a VRAM fica em 11,4-11,6 GB de 12,8 GB com `collecting=1` dos 90 s até o fim.

`KYTY_PIPELINE_WAIT_SUSPEND=1` (desligado por padrão, ligado no `Play.bat` da `fase1` até uma
partida validar; `pipelineCache.cpp/.h`, `graphicsRun.cpp`, `commandProcessor.h`): o draw que espera
o pipeline espera 4 ms por vez; entre uma fatia e outra o pacote é suspenso (`SuspendForPipeline`),
a GPU thread atende a fila e roda o pacote de novo, sem marcar a submissão como bloqueada. O prazo
total continua 3 s / 10 s (`PendingGraphicsPipeline::deadline`, armado uma vez só: a versão do
Codex o empurrava a cada lookup, e um draw suspenso esperaria para sempre), e passado o prazo o draw
é pulado como antes. Num multi-draw indireto o pacote retoma do draw que suspendeu
(`Pm4Execution::m_multi_draw_packet/_next`): os anteriores não se repetem. Cada nova tentativa refaz o
preparo do draw (bindings, uploads), que já é seguro porque o caminho do draw pulado faz o mesmo.

Audit de 24/10 (espera da GPU, pacotes PM4, draws), correções do mesmo build:
- `KYTY_COMPUTE_ASYNC=1` (desligado por padrão, ligado no `Play.bat`; precisa do
  `KYTY_PIPELINE_WAIT_SUSPEND=1`): compute novo que o cache do driver não tem compila num worker
  (`CreatePipelineInternal` com `deferred`, `PipelineCompiler::Run`), e o dispatch espera em fatias de
  4 ms com o pacote suspenso entre elas, como o draw. Nunca é pulado e não tem prazo. O lookup
  (`TryGetComputePipeline`) foi para antes de qualquer gravação do dispatch (antes do
  `EndRendering` e da conversão dos argumentos indiretos), para o pacote poder rodar de novo. Fora de
  um pacote que pode repetir, compila como antes. O compute compilado no worker não passa pelo
  `CompileWatch` (lista de pipelines lentos).
- `[pipeline-wait]` no stderr (sempre ligado, só log, as primeiras 200): cada espera da GPU thread por
  um pipeline acima de 100 ms, com hashes, palavras SPIR-V, motivo (`writes-memory`, `small-target`,
  `layer`, `small`, `background`, `dispatch`), se é sem otimização, e quanto foi fila e quanto foi
  compilação.
- Retirado do audit: o "poll de 100 ms" do loop bloqueado não existe; `CondVar::WaitFor` conta em
  microssegundos (`threads.h:75`), então são 100 µs. `KYTY_READBACK_ASYNC_LIFT` já é padrão na tabela
  do `main.cpp`; não precisa ir no `Play.bat`.
- O Codex (01:09) trocou só um comentário do `shaderDiskCache.h` (leituras concorrentes do `Store`), e
  isso já mudava a versão do cache (0x372ebaba → 0xc81b40c0): o hash cobre os bytes dos seeds e dos
  headers que eles incluem, comentários e fins de linha também. O comentário voltou ao do HEAD (com
  CRLF), e a nota foi para a classe `DiskReaders` no `pipelineCache.cpp`. Antes de instalar, conferir
  `SourceVersion` em `_Build/port-lto/shaderCacheVersion.h`.
- Revisão do código do Codex (mesmo build):
  - `PipelineCompiler::Warm` e o fim de um aquecimento acordam todos os workers (`notify_all`): só os
    de fundo pegam aquecimento, e um `notify_one` que acordasse outro se perdia, deixando o
    aquecimento parado até a saída (que o descarta).
  - `KYTY_SHADER_READ_THREADS` agora vem desligado (padrão 0; o Codex deixou 4): leitura em andamento
    pula o draw 3D no primeiro uso de cada shader em toda partida, inclusive draw que escreve memória.
    Fica no `Play.bat` como `rem` para testar depois, sozinho.
  - `KYTY_PIPELINE_QUICK`: na saída o worker não passa mais o quick como final (a thread da GPU podia
    já tê-lo pego, deixando o pipeline nulo); o `FinishPending` compila na hora.
  - Linha de log das leituras com `{}` no lugar de `%`.
- Partida de 24/10 01:53 (build 01:53, `WAIT_SUSPEND` e `COMPUTE_ASYNC` ligados): fechou aos ~52 s,
  entrando no gameplay, com `unknown op`, `cmd_id = ffffffff` no offset 0x1c de um buffer PM4
  (`graphicsRun.cpp`). Erro novo, mas sem prova de ligação com a suspensão: nenhuma linha
  `[pipeline-wait]` (nenhuma espera acima de 100 ms terminou), e as duas partidas anteriores (build
  00:51, sem essas chaves) também fecharam entre 55 e 72 s, com outros erros. Mesmo assim, a suspensão
  deixava as outras filas rodarem enquanto uma esperava o pipeline, o que o console nunca faz por
  segundos (o jogo pode reaproveitar memória de comando por isso). Agora (`ThreadRun`,
  `pipeline_queue`) a submissão que espera volta antes de qualquer outra fila; entre as fatias só os
  comandos da thread da GPU (as leituras do jogo) são atendidos, como na espera de antes. O erro de
  pacote desconhecido agora sai no stderr como `[pm4-unknown]` (fila, buffer, tamanho, offset,
  quantas suspensões por pipeline e por SRT a submissão teve, e o dump).
- Partida de 24/10 ~02:03 (build 02:01, mesmas chaves): fechou aos ~76 s com erro de acesso na
  MainThread do jogo, `pc=0x903dcb21d`, escrita em `0xffffffffffffffff`. O código faz
  `mov rcx,[r13+0x28]; mov rcx,[rcx+0x20]; mov word [rcx+0x50],0`, e `rcx = 0x3f91aa53412d4a37`, que
  são dois floats num campo de ponteiro: memória do jogo corrompida. É a mesma família das quedas de
  23/10 00:35 (pc 0x903d469c3, exe mais antigo) e 24/10 01:15 (build 00:51, sem as chaves novas),
  então provavelmente não vem da suspensão. Suspeitos ainda sem prova: a emulação de leitura de
  volta (readback / clean-read), escrita da GPU de volta em faixa errada, streaming AMPR. Nessa
  partida a suspensão funcionou para as leituras do jogo: 71 `[readback-slow]`, nenhuma acima de
  500 ms, fila máxima de 101 ms (contra 19 acima de 500 ms e fila de 2,1 s na partida de 00:53). As
  `[pipeline-wait]` mostraram esperas de 385 a 1581 ms, até 875 ms delas na fila (todos os workers
  ocupados); compilações sem otimização de 500 a 700 ms em shaders de 85 a 106 mil palavras.
- Build 02:08: o relatório de queda do jogo (`runtimeLinker.cpp`) agora mostra, depois da pilha, os
  64 bytes atrás de cada registrador e mais um nível atrás de cada qword que seja ponteiro legível. No
  Windows, uma página reservada mas protegida aparece como `committed, protect=0x.., not readable`
  (mostra se a página está sob proteção do emulador).
- Partida de 24/10 ~02:10 (build 02:08): fechou aos ~67 s na Render Thread, `pc=0x903d469c3` (o
  mesmo pc de 23/10 00:35). Uma função do jogo devolveu `rax = 0x3f800004377fffff` (floats) como
  ponteiro de um registro de 16 bytes. A lista de blocos em `0x12c44021c0` tem pares
  (`0x007c000000000241`, bloco), e o último bloco, `0x12c4415340`, começa com floats (1.0, 0.45, 0.5,
  18, 4, 8, 0.7) em vez de registros. Não é memória estendida (AMM fica em 0x80000000000).
- Build 02:18 (instalado; anterior em `kyty_emulator.0208.exe`): anel das últimas 65536 escritas que o próprio emulador faz na memória do jogo
  (`Memory::NoteEmulatorWrite`): `B` dado da GPU devolvido (`TryWriteBacking`, buffers e imagens),
  `F` leitura de arquivo do AMPR, `D` cópia DMA feita na CPU (`BufferCache::CopyBuffer`). O dump de
  ponteiros da queda mostra a faixa do jogo de cada ponteiro e as últimas 4 escritas do emulador nos
  64 bytes atrás dele (`emulator wrote it: ...`). Se a próxima queda mostrar uma escrita ali, a
  culpa é do emulador e o tipo diz qual parte.

A medir: o `[queue ...]` das linhas `[readback-slow]` durante os congelamentos, e as linhas
`[pipeline-wait]`.
