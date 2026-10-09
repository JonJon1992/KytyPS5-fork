# Revisão: F4, F2 e memo bindless — 8 de outubro de 2026

Commits revisados (Codex, branch cpu-coherence-service):

- 3b11ab60: F4 (candidatos finitos de escrita BDA) e F2 (uploads de leitura no worker);
- 07b069df: memo de texturas ampliado para bindless.

As correções estão no branch coherence-review-fixes, sobre 57f0d977 (F3).

## Correções

### Memo de texturas (07b069df)

- **Capacidade pelo dispositivo, não pela variável.** O memo lia
  `KYTY_BINDLESS=1` direto do ambiente. O bindless só fica ativo com
  `GraphicContext::bindless_supported`, que exige as features de descriptor
  indexing e pode ser desligado pelo orçamento de descritores
  (vulkanWindow.cpp, `CalculateBindlessBudget`). Antes, um dispositivo que
  recusasse o bindless alocava os 62 MiB mesmo assim. Agora o
  RenderExecutor passa `bindless_supported` ao construtor do memo.
- **Override testável.** `TextureBindingMemo::SelectCapacity` virou
  função pública e pura. O teste `texture_memo_capacity*` cobre
  valores válidos e inválidos (`""`, `0`, `512`, `3000`, `131072`, `4096x`,
  `" 4096"`, `-4096`, `abc` e estouro de 32 bits). Também confere que o memo do
  executor usa 65.536 posições quando o dispositivo roda bindless.
- Ordem dos includes e chaves no estilo do arquivo.

### F4 (3b11ab60)

- **Destino na mesma página da tabela.** O Prepare reservava os destinos com
  `ObtainBuffer(..., true)` antes da última checagem. Essa reserva marca páginas
  inteiras do tracker (4 KiB) como da GPU. Um destino que dividisse página com a
  tabela deixava a tabela GPU-modified. O Finalize recusava o dispatch depois da
  reserva, e todo dispatch seguinte também era recusado enquanto a página
  continuasse da GPU. Agora o Prepare recusa esse caso antes de qualquer
  mudança de dono. O teste nativo `bda_candidates_cp*` cobre isso: a tabela
  recusada não reserva nenhum destino.
- **Motivo de cada rejeição.** O log dizia "lacks a complete proof" em
  todos os casos, e a recusa do Finalize não gerava log. Agora `Plan::reject`
  guarda o motivo, e um helper único (`RejectBdaWriteCandidates`) conta e
  registra os dois pontos, nos dispatches diretos e indiretos. Exemplos de
  motivo: tabela GPU-modified, descritor sem limite, destino não mapeado,
  página compartilhada, mapeamento alterado. É o dado que falta para o A/B
  das admissões no jogo.

### Ferramenta

- `shader_cfg_tests --structurize-file` usa o hash do nome do arquivo
  (`<16 hex>.bin`). Assim as opções por shader (`KYTY_BDA_WRITES=candidates`,
  `KYTY_BDA_WRITES_SHADERS`) valem na tradução offline, como no emulador.

## Conferido sem mudança

- **Resolve × emitter.** As faixas de `BdaWriteCandidates::Resolve` cobrem o
  que `StoreIndirectBuffer` pode gravar com o store auditado (IDXEN DWORD,
  offsets zero), nos quatro OOB_SELECT. Formato 0 e `num_records` 0 não gravam.
  OOB 0 com stride 0 não grava. OOB 1 e OOB 3 sem stride gravam só em `base`. Com
  stride ≥ 4, a última escrita termina dentro de `stride·num_records`. OOB 2,
  OOB 3 com stride, swizzle e stride < 4 são recusados.
- **Salvar/congelar/restaurar.** `Buffer::CopyFrom` ordena as leituras do
  dispatch antes da restauração (barreira AllCommands → Transfer) e a
  restauração antes do próximo uso do backup. Com o CP recorder, o `Handle()`
  da restauração drena o recorder depois do dispatch, então a ordem se mantém.
- **F2.** Só as cópias de guest vão ao worker. Hot pages, a reserva
  temporária e as cópias gravadas seguem inline. O limiar compara só os bytes
  de guest. As fontes são aliases estáveis e a staging não-coerente recebe flush
  do worker. No modo sem batching, as barreiras equivalem às do caminho
  síncrono.

## Itens pendentes da primeira rodada, agora corrigidos

- **Escritas perdidas sem aviso no modo candidates** (5c961460). O contador
  continua no shader. Depois de cada dispatch candidato,
  `FaultManager::QueueBdaDroppedCheck` copia o contador para um slot de readback e o
  zera, sem espera. O valor é lido quando a gravação termina: conta
  `BdaCandidateDroppedWrites`, gera log e é fatal com `KYTY_BDA_WRITES_VERIFY`.
  Teste: `BdaCandidatesDroppedCounted` (64 escritas numa página sem buffer).
- **Drenagens do recorder** (5c961460). `Buffer::CopyFromEncoded` grava as mesmas
  barreiras e a mesma cópia pelo `Sink()`. Salvar, congelar e restaurar a tabela não
  drenam mais o CP recorder.
- **PrepareBindlessHeaps e TryResolve** (89585371). Toda chave continua sendo
  resolvida (touch, residência, refresh). Uma chave cujo slot ainda guarda a view
  resolvida da mesma imagem mantém slot, tradução e referência. Isso elimina o
  `ReleaseKey`, as duas escritas de tradução, o `FindSlot` e o `AddImageReference`
  (≈15% do Thread_Gpu no perfil). Também:
  - o memo recebe o hash e a tag do mesmo T# como dica (sem hash nem comparação de
    chave);
  - o `FindTexture` vira `TryAcquireView`;
  - o `BindlessTexture` guarda só imagem, layout e faixa da view.
  Contadores: `BindlessHeapKeys` e `BindlessHeapKeysKept`.
  Teste novo: `bindless_heap_repeat`. Com `KYTY_BINDLESS=1` o harness agora liga a
  tabela bindless.

## Revisão de F1, F3 e da integração da fase 0

- **Integração da fase 0 (4c63d476):** os 54 trechos foram reaplicados idênticos.
  O settle não escreve memória do guest, e a espera dele já inclui a dependência do
  copier. Sem defeito.
- **F1 (9956c566):** sem defeito confirmado. Corrigido em 8d1d74e1:
  - as cópias em host do UploadDma ficavam fora do guard de escrita do emulador. É
    uma lacuna anterior à F1. O UploadDma agora rastreia as fontes num
    `SourceCopyTracker`, os dois guards consultam os dois copiadores, e a admissão
    usa o mesmo gate da F1. No RADV da RX 9070 XT não há fila só de transferência,
    então o UploadDma não roda aqui (o teste reporta "skipped");
  - os modos de diagnóstico do `GET_LOD_STATS` escreviam sem `BeforeEmulatorWrite`;
  - dois produtores simultâneos no `StagingCopier` agora são fatais (flag
    atômica), em vez de embaralhar os tickets;
  - o verify avisa quando não classifica mudanças de fonte (sem
    `KYTY_BDA_INCREMENTAL_SYNC`).
- **F3 (57f0d977):** sem defeito de corretude atingível. Corrigido em 8d1d74e1:
  - o lote do pool se perdia quando o `DetachTo` não tinha o que destacar, e o
    reuso era contado mesmo assim;
  - um coletor adiado agora emite a faixa de um watch ainda não aplicado (antes,
    um escopo que terminasse de forma síncrona podia copiar uma página ainda sem
    proteção).

## Pendências

- **Teste nativo de F2 + F3 juntas.** Com `COPY=read` e `PROTECT=1`, o fixture só
  usa o caminho BDA. A prontidão de um consumidor síncrono durante a janela é
  coberta só no PageManager (`page_manager_protect_readiness`).
- **Fila FIFO da F3.** O prefixo de proteção espera jobs de textura e de F1/F2 já
  enfileirados. Medir `CoherenceProtectWait` e `CoherenceCopyGuard` no A/B antes de
  pensar em prioridade.
- **Diagnóstico.** `BeginProtectProbe`/`EndProtectProbe` contam só as chamadas da
  thread que chamou, e as do worker da F3 não aparecem.
- **TryResolve com 65.536 posições.** O caminho bindless agora chama o memo com
  dicas e sem `FindView`. A próxima captura do Yōtei diz quanto do custo
  sobrou e se ainda vale mudar o layout da tabela.
- **A/B controlado e program cache:** como antes.

## Validação

Build Release (clang, `_Build/rf` do worktree kyty-review-fixes) de
`kyty_emulator`, `shader_recompiler_compute_tests`, `shader_cfg_tests`,
`page_manager_tests` e `source_copy_tracker_tests`, sem warnings novos. Testes
rodados um por vez (limite de 12 GB), todos passando:

- memo: `texture_memo_capacity`, `texture_memo_capacity_bindless`,
  `texture_memo_revalidate`, `_verify`, `_off` e `_large`;
- F4 e fase 0: `bda_candidates_cp0`, `bda_candidates_cp1` e `bda_writes`;
- F2: `coherence_copy_tex0_read` e `coherence_copy_read_unbatched`;
- F4 (segunda rodada): `bda_candidates_cp0/1` com `BdaCandidatesDroppedCounted`;
- bindless: `bindless_heap_repeat`;
- F1/F3: `source_copy_tracker`, `page_manager*`,
  `coherence_protect_{1,read}_rec{0,1}`,
  `coherence_copy_tex{0_1,1_read,0_verify}` e `coherence_copy_read_unbatched`.

No harness, `KYTY_BINDLESS=1` não ativa a tabela bindless. O teste
`texture_memo_capacity_bindless` confirma o caso corrigido: só o pedido, sem a
tabela, mantém 4.096 posições.

Tradução offline dos dois escritores reais do Yōtei (binários em
`_Build/coherence-f1/validation/real-writers/`, `KYTY_SRT_VARIANT_READS=1`,
`KYTY_STRUCTURIZE_FILE_SPIRV=1`):

| Modo | 86da5eb7b8257bb0 | d8959888aafd2552 |
| --- | --- | --- |
| sem `KYTY_BDA_WRITES` | skip_dispatch=1 (pc 0x530) | skip_dispatch=1 (pc 0x1b4) |
| `candidates` | skip_dispatch=0, 2 imagens | skip_dispatch=0 |
| `candidates-verify` | skip_dispatch=0, 2 imagens | skip_dispatch=0 |

Ou seja, tornar dinâmicas todas as leituras escalares desses shaders não
deixou nenhum store estático sem resolver. A admissão no jogo continua
dependendo das tabelas reais: os motivos agora aparecem no log
(`BDA candidates: shader=... skipped: <motivo>`).
