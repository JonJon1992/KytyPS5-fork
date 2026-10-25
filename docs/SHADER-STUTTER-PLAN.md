# Engasgos de shader no Wolverine: análise e plano (2026-10-22)

Base: KytyPS5-Frank v0.3 (branch `wolverine-port`). Máquina de referência: RX 9070 XT, Ryzen 7 5700X,
Windows. Medido em jogo (Frank v0.3): 23–24 FPS estável; 0–2 FPS ao entrar em área nova, com o log
mostrando dezenas de shaders novos por segundo (ex.: +94 PS e +77 CS em 15 s). Processo usa ~2,6
núcleos de 16 threads: há CPU ociosa para compilar em paralelo.

## Caminho de um shader novo hoje

| Etapa | Onde | Thread |
|---|---|---|
| Tradução (decode, CFG, IR, rastreamento de recursos) ~85 ms | `pipelineCache.cpp:2065-2088` | gráfico 3D: worker (draw pulado); **compute: GPU thread** (`:1076`) |
| Materialização + emissão SPIR-V | `pipelineCache.cpp:2141` (`CompileTranslated`) | GPU thread |
| Módulo Vulkan | `AddPermutation`, `:1370` | GPU thread |
| Pipeline gráfico | `TryGetGraphicsPipeline`, orçamentos em `:3677` | worker + espera na GPU thread |
| Pipeline compute | `GetComputePipeline`, `:3729-3751` | **GPU thread, síncrono** |
| Cache de tradução em disco | `_ShaderCache/<título>` (por entrada, gravado durante o jogo) | — |
| Cache do driver (VkPipelineCache) | `_PipelineCache/<título>.bin`, `Save()` em `:2986` | **só ao sair** |

## Gargalos reais

1. **Espera fixa por pipeline gráfico** (`g_pipeline_wait`, 20 ms, `:932`): cada pipeline novo em
   segundo plano para a GPU thread por até 20 ms. 100 pipelines numa troca de área = até 2 s.
2. **Pipelines "síncronos" esperam até 3 s / 10 s** (`:3662-3677`): pequenos (≤12 000 palavras,
   `:882`), de interface/camada e de draws que escrevem memória. Correto para compatibilidade (draw
   feito uma vez só), mas cada compilação do driver da AMD trava o frame.
3. **Compute 100% síncrono**: tradução ~85 ms + emissão + pipeline na GPU thread, um por vez.
   Pular dispatch não é seguro (dados não escritos), então hoje não há paralelismo nenhum.
4. **Cache do driver salvo só na saída**: o jogo tem device lost a cada 20–40 min (README do Frank).
   Um crash perde todos os pipelines compilados na sessão; a próxima execução recompila tudo.
5. **Poucos workers**: 2 tradutores (`:2183`), `clamp(hw/4, 1, 4)` compiladores (`:2882`).
6. **Chave de pipeline grande**: blend, máscara de cor, topologia, polygon mode, amostras e vertex
   input são estado estático (`pipelineCache.h:46-106`); cada combinação é um pipeline a compilar.
   Só cull/face/depth bounds viraram dinâmicos (`NormalizeGraphicsPipelineKey`, `:909`).
7. **Sem pré-compilação**: nada é preparado antes do jogo pedir; a 2ª visita depende de acertos de
   cache (rápidos, mas ainda na GPU thread, um por draw).

## Mudanças propostas (ordem de implementação)

Tudo atrás de variáveis `KYTY_*`; ligado por padrão só depois de validado em jogo.

### Fase 1 — rápidas, baixo risco
1. **Orçamento de espera por janela** (feito): `KYTY_PIPELINE_WAIT_MS` vale por janela de
   `KYTY_PIPELINE_WAIT_WINDOW_MS` (33 ms), não por pipeline. Pior caso por frame: 20 ms.
2. **Sonda do cache do driver sem bloquear**: criar o pipeline primeiro com
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` (Vulkan 1.3 core,
   `pipelineCreationCacheControl`). Acertou no cache → usa na hora, sem pular draw nem esperar.
   Precisa compilar → vai para o worker (gráfico em segundo plano: pula já, sem os 20 ms).
   Vale para compute também (acerto = custo ~zero).
3. **Salvar o cache do driver periodicamente**: thread que grava `_PipelineCache` a cada ~2 min se
   houve pipeline novo (arquivo temporário + renomear), e no caminho de erro fatal.
4. **Workers pela quantidade de núcleos** e **prioridade abaixo do normal** para tradutores e
   compiladores, para não roubar CPU da GPU thread e das threads do jogo durante as rajadas.

### Fase 2 — compute e pipelines síncronos
5. **Compilação em dois níveis**: quando é obrigatório compilar na hora (compute, interface,
   draws que escrevem), compilar primeiro com `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`
   (bem mais rápido no compilador da AMD), usar imediatamente e enfileirar a versão otimizada;
   trocar quando ficar pronta. Mesmo SPIR-V, mesmo resultado: compatibilidade preservada.
6. **Pré-tradução antecipada de compute**: o processador de comandos já varre à frente
   (scanner de draw-prep, CpSequencer). Ao ver `COMPUTE_PGM_LO/HI` + dispatch adiante, enfileirar a
   tradução (e, com o SPIR-V, o pipeline) nos workers pela chave exata de origem. A GPU thread só
   espera o que faltar. Sem pular dispatch nenhum.

### Fase 3 — menos pipelines (reutilização)
7. **Extended dynamic state 3** (se suportado pelo driver): blend enable/equation, color write
   mask, polygon mode, topologia irrestrita, amostras de rasterização saem da chave.
8. **`VK_EXT_vertex_input_dynamic_state`**: strides/offsets saem da chave.
9. **Graphics pipeline library** (experimental): VS e PS compilados uma vez como bibliotecas,
   link rápido na hora e link otimizado em segundo plano. Ficou desligado no nosso fork por bugs
   no Crash 4; testar com cuidado.

### Fase 4 — pré-compilação inteligente
10. **Banco de estados de pipeline**: gravar em `_PipelineCache/<título>.keys` a chave completa de
    cada pipeline usado (gráfico e compute) + chaves de permutação do cache de disco, com a ordem do
    primeiro uso. Na abertura (durante intros/menus), workers recriam os módulos a partir do
    `_ShaderCache` (que já guarda o SPIR-V) e criam os pipelines com o cache do driver, na ordem de
    uso. Pedidos do jogo têm prioridade sobre o aquecimento. Resultado esperado: a 2ª visita a
    qualquer área sem compilação nenhuma.

### Específico AMD RDNA
- O custo dominante é o compilador do driver: a sonda (2), o nível sem otimização (5) e menos
  pipelines (7–9) atacam exatamente isso.
- Compute: pedir `requiredSubgroupSize = 64` quando o shader do PS5 é wave64 (já feito para PS e
  mesh), evitando o driver gerar wave32 + emulação.
- `VK_EXT_pipeline_creation_feedback` para medir acerto de cache por pipeline (diagnóstico).

## Fora dos shaders: liberação de memória do streaming (feito, `KYTY_ASYNC_UNMAP`)

Teste com cache cheio (2026-10-22): 51 lotes de unmap do AMM em 2 min, ~20 ms cada ("GPU-side
unmaps"). Causa: `RenderContext::UnmapMemory(Ranges)` mandava um comando síncrono à GPU thread, que
chamava `CommandScheduler::Finish()` (submete e espera a GPU inteira) antes de invalidar os caches.
Agora a GPU thread só submete; quem libera (motor do AMPR) espera o tick e as operações prioritárias
(escritas na RAM do jogo) e depois a GPU thread invalida os caches, na mesma ordem de antes.
Buffers e imagens já eram destruídos só depois da GPU terminar (`DeferRelease`). Memória importada
(shared buffers / host zones) continua drenando: a especificação exige que a memória do host
continue válida durante toda a vida do import (`VK_EXT_external_memory_host`).

## Gerenciamento de VRAM (feito, 2026-10-22)

Perfil com símbolos (build LTO) aos 150 s de trajeto, 2 FPS: ~27% da GPU thread criando e destruindo
buffers (`RebindBuffers` → `Buffer::Buffer` → VMA → driver esperando em
`NtGdiDdDDIWaitForSynchronizationObjectFromCpu`; `FreeDedicatedMemory` nas destruições). Causas e
mudanças, cada uma com switch:

| Mudança | Switch (padrão ligado) | Onde |
|---|---|---|
| Buffers BDA ≤64 MiB em blocos compartilhados do VMA (não uma alocação de kernel por buffer) | `KYTY_BDA_SHARED_BLOCKS=0` | `streamBuffer.cpp` (do nosso fork) |
| Histerese: coleta ao passar de 90% do orçamento, até 82% | `KYTY_VRAM_HYSTERESIS=0`, `KYTY_VRAM_HIGH_PCT`, `KYTY_VRAM_LOW_PCT` | `vma.cpp` `MemoryShortfall` |
| Idade mínima em tempo (buffers 0,5/1,5 s; texturas 0,5/1/2 s), não só em frames | `KYTY_GC_MIN_AGE=0` | `gcAge.h`, coletores |
| Coleta de buffers limitada a 1 ms por passada (limpeza gradual) | `KYTY_GC_TIME_BUDGET_US` | `bufferCache.cpp` |
| Buffers escritos pela GPU: download sem esperar a GPU; liberados numa passada seguinte se não reusados | `KYTY_GC_ASYNC_DIRTY=0` | `bufferCache.cpp` |

### Coletor de texturas (análise do código, 2026-10-22)

Já estava correto e foi mantido: liberação só depois do tick do último uso na GPU
(`ReleaseImage` + `IsFree(tick_accessed_last)` + `DeferRelease`, via timeline semaphore); write-back
de imagens escritas pela GPU assíncrona (operação prioritária + flag `landed`, "revive" se usada de
novo); nenhuma espera global da GPU no coletor. Faltava e foi feito:

| Mudança | Switch (padrão ligado) | Onde |
|---|---|---|
| Histerese nos limites do próprio cache: sob pressão coleta até ~8% abaixo do limite | `KYTY_GC_HYSTERESIS=0` | `textureCache.cpp` |
| Texturas só amostradas criadas dentro do orçamento; além dele vão para a memória do host em vez de forçar paginação (render targets e storage mantêm a regra antiga, como o VMA recomenda) | `KYTY_IMAGE_WITHIN_BUDGET=0` | `vma.cpp` `CreateImage` |
| Pool de imagens liberadas (até 256 MB, 10 s; vazio sob pressão): uma imagem de mesmo formato/tamanho/uso volta sem nova alocação no driver; só imagens que o cache liberou depois da GPU terminar | `KYTY_IMAGE_POOL_MB=0`, `KYTY_IMAGE_POOL_AGE_S` | `vma.cpp`, `TextureCache::ReleaseImage` |

Base no Context7: VMA aloca além do orçamento por padrão e deixa a decisão ao driver;
`WITHIN_BUDGET` para recursos não essenciais; recurso reaproveitado deve ser tratado como não
inicializado e começar em `VK_IMAGE_LAYOUT_UNDEFINED`, o que a especificação do Vulkan sempre permite
(conteúdo descartado).

## AMD (RX 9070 XT, driver Windows) — o Frank foi feito numa NVIDIA

Medido/conferido nesta máquina:
- Filas (vulkaninfo): família gráfica com 8 filas, compute com 8, transfer (SDMA) com 1. A segunda
  fila da família gráfica existe, então as cópias de volta pela fila de cópia (`readback_queue`)
  funcionam aqui como na NVIDIA.
- Apresentação: o Frank trocava Mailbox por Fifo na AMD (Mailbox dá janela preta no driver
  Windows). Com Fifo o FPS ficava preso em 60/n (20, 15, 12, 10). Agora o padrão na AMD é
  Immediate (sem tearing dentro da faixa do FreeSync); `KYTY_AMD_PRESENT=fifo` volta ao Fifo.
- Driver: consultar o orçamento de VRAM (`vkGetPhysicalDeviceMemoryProperties2` via
  `vmaGetHeapBudgets`) espera no kernel; com alocação dedicada por buffer isso acontecia a cada ~30
  alocações. Resolvido em parte pelos blocos compartilhados.
- O gargalo medido é CPU (thread de renderização do jogo + thread da GPU do emulador); a GPU sobra.

Próximos itens AMD, a medir antes de virar padrão:
- Compute em wave64 nativo (`requiredSubgroupSize = 64`) quando o shader do PS5 é wave64.
- Anel de upload em memória BAR/ReBAR (o nosso fork tem `docs/performance/UPLOADS-REBAR-2026-10-09.md`).
- Barreiras e layouts que desligam a compressão DCC (o nosso fork tem `BARRIERS-*` e `metadata-clear-eliminate.md`).

## Tempo de thread (perfil de 2026-10-22, rota normal, ~15–22 fps)

Perfil aos 60 s e 150 s com o `SlotVector` original (a versão em blocos travou o jogo numa rodada
e foi desfeita: variantes de compute 2830 contra ~300, dispatches com argumentos lixo, zonas dos
marcadores derrubadas; nenhum erro de lógica achado, ficou de fora até outro teste A/B).

- **Coletor de texturas: 12 % da thread da GPU** (`TextureCache::RunGarbageCollector`). Roda a cada
  submissão (centenas por quadro); sob pressão cada passada percorre até 4096 entradas da LRU, e se
  nenhuma pode sair (alvos escritos pela GPU além do orçamento de write-back, profundidade, alvos
  pequenos) a passada não gasta o orçamento do quadro e a submissão seguinte repete tudo. Cada
  entrada é uma falta de cache na lista e outra no slot da imagem (`std::deque` do MSVC).
  `KYTY_GC_IDLE_MEMO` (padrão ligado; `=0` como antes): uma passada que não liberou nada não se
  repete até o relógio da LRU andar (ou a pressão subir de nível); alvos pequenos e associações de
  profundidade, que este coletor nunca libera, vão para o fim novo da LRU; a lista de texturas
  largadas pelo jogo é varrida uma vez por quadro.
- **Leituras de memória escrita pela GPU: 40–50 % das threads do jogo** (`HandleFault` →
  `BufferCache::ReadMemory`). Por leitura: ~45 % esperando a fila principal chegar ao tick, ~26 %
  esperando a cópia da fila secundária (`vkWaitForFences`), ~19 % na ida e volta à thread da GPU
  (`SendCommandSync`, duas por leitura). O log mostra a fila secundária quase sem uso
  (`writers_running=2049 taken=19`): os escritores são conhecidos por bloco de 1 MiB, e um bloco
  da janela de 4 MB escrito no quadro atual mandava a janela inteira para a fila principal, atrás
  de tudo o que foi gravado. `KYTY_READBACK_NARROW_COPY` (padrão ligado; `=0` como antes): nesse
  caso o bloco de 1 MiB da leitura, se os escritores dele já terminaram, é copiado sozinho na fila
  secundária; o resto da janela fica protegido para uma leitura posterior. Contador no log:
  `[copy-readback] narrow taken=… block_running=…`. Só para leituras das threads do jogo: na
  primeira versão valia também para o refresh de SRT da thread da GPU (`StartSrtReadback`), que lê
  os bytes antes de pedir a leitura e seguia com os antigos quando a cópia terminava na hora; o jogo
  fechou na abertura (`descriptors.cpp:3501`, bindings preparados para outra variante do shader).
  O caminho de cópia da janela inteira tem o mesmo defeito latente, mas quase nunca é tomado.
- **Resultado (rodada de 2026-10-22 14:05, build limpo):** gameplay de 9,4 para 13,7 fps de média,
  segundos abaixo de 5 fps de 68 para 9; coletor de texturas de 12,9 % para 1,0 % da thread da GPU;
  leituras nas threads do jogo de ~41/38 % para ~35/29 %. Os dois switches viraram padrão no
  `Play.bat`. Dentro da leitura, a maior parte agora é a ida e volta à thread da GPU
  (`SendCommandSync`, 49–66 %), que só atende entre pacotes PM4; cada leitura fazia duas, a segunda
  só para tirar a proteção da janela.
- `KYTY_READBACK_ASYNC_LIFT=1` (opt-in, `Play-teste.bat`): depois da publicação, se os bytes da
  leitura estão atuais na memória do guest, o pedido de tirar a proteção vai sem esperar e a leitura
  volta; repetida antes disso, a emulação de leitura limpa a completa pela cópia de apoio. Escritas
  continuam esperando. A medir: cada leitura emulada custa uma exceção.
- **Build:** o `_Build/port-lto` tinha o ccache como launcher e 35 objetos sem dependências de header
  (`#deps 0`): mudanças em `textureCache.h`/`bufferCache.h` recompilaram 5 de ~40 arquivos e o
  executável misturou layouts (fechamento na abertura, congelamento numa trava com dono 0xFFFFFFFF).
  Reconfigurado com launchers vazios e recompilado do zero; o script de build agora recusa instalar
  se `ninja -t deps` mostrar algum objeto com 0 dependências.

## Leituras na fila SDMA e os blocos amarelos no vidro (2026-10-22)

- `KYTY_READBACK_QUEUE=transfer` (+ `KYTY_READBACK_VERIFY=1`, o `Play-teste.bat`) é o modo mais
  rápido medido: no primeiro minuto de gameplay, 18,8 e 21,1 fps contra 13,2 e 15,2 do `Play.bat`
  (mesmo save). Em rodadas longas cai mais no fim da rota (média de 240 s: 13,0 contra 14,5).
- Defeito visual só nesse modo: blocos amarelos nas janelas do corredor. Os blocos têm bordas
  alinhadas à tela (não ao plano do vidro), unidades de ~16 px, e só no vidro (transparente,
  iluminação forward): parece um buffer por tile da tela (lista de luzes, por exemplo) com partes
  velhas, não uma textura do disco. A verificação acusou 3 cópias com bytes velhos em 17 mil (o jogo
  recebe a cópia conferida), então a causa provável não são os bytes das leituras.
- Diferenças do modo: cópias no SDMA, buffers `CONCURRENT`, ticks de escrita mais conservadores
  (`NoteAddressWrites`, binding inteiro, +1 tick) e, com a verificação, um `Flush` por cópia.
- Teste ao vivo: no jogo, **F9** desliga/liga a verificação, **F10** desliga/liga as cópias na fila
  de leitura (as recusadas vão para a fila principal, como quando os escritores ainda rodam). Cada
  tecla escreve `[diag-key] ... OFF/ON at HH:MM:SS` no log.
- Resultado (18:45): nenhuma das duas tira o amarelo, e o fps no corredor fica em 19–20 com as cópias
  ligadas ou não. Ampliados, os blocos são retângulos alinhados à tela, com bordas pontilhadas,
  amarelos ou esbranquiçados: um efeito de tela refeito a cada quadro. Sobra o que é fixo desde a
  criação do dispositivo: buffers `CONCURRENT` e a fila extra de transferência. Desligar a verificação
  com as cópias ligadas fechou o jogo em 2 s (`ud2` do próprio jogo na thread "Job Hi 2"): bytes velhos.
  O ganho do primeiro minuto não é prova do modo: as rodadas do `Play.bat` são de um build e caches
  mais antigos. Próximo: `Play.bat` com o build atual, no mesmo corredor.

## Premissas conferidas na especificação (Context7, `/khronosgroup/vulkan-docs`, cppreference)
- Sinal de semáforo: o primeiro escopo inclui todos os comandos anteriores na ordem de submissão, e o
  escopo de acesso é toda memória acessada pelo dispositivo; a espera em outra fila cobre os comandos
  seguintes no estágio pedido (transferência). A cópia da fila secundária enxerga as escritas dos
  escritores terminados; as duas filas são da mesma família, sem transferência de posse.
- `FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`: o driver não pode compilar; devolve
  `VK_PIPELINE_COMPILE_REQUIRED` e pipeline nulo; exige `pipelineCreationCacheControl` ativado.
- `VkPipelineCache` só exige sincronização externa com `..._EXTERNALLY_SYNCHRONIZED_BIT`.
- Semáforo de timeline: espera no host de qualquer thread, sem exigir sinal antes da espera.
- `std::condition_variable::wait_for` com predicado devolve `false` só no timeout com predicado
  falso; `std::thread` ainda "joinable" no destrutor chama `std::terminate` (o saver faz `join`).

## Como validar (quando houver teste em jogo)
- Mesma rota, mesma área nova, com e sem cada switch: frame time p99, draws pulados, tempo da GPU
  thread esperando compilação (contadores `PipelineBuildTime`, `PipelinesPending`).
- Segunda execução na mesma rota: deve ficar sem rajadas de compilação (fases 1.2 + 4).
