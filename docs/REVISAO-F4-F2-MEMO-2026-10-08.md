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

## Pendências (sem mudança neste branch)

- **Escritas perdidas sem aviso no modo candidates.** O emitter tira
  `RecordBdaDroppedWrite`. Se uma página de destino ficasse sem entrada na
  tabela BDA, a escrita sumiria com a página marcada como da GPU. Pela
  construção (ObtainBuffer registra o buffer), isso não deve ocorrer; só o
  modo `candidates-verify` detecta.
- **Duas drenagens do recorder por dispatch candidato** (Finalize e
  Restore usam `Handle()`). Custo só nos dois shaders.
- **TryResolve em 19,62% do Thread_Gpu com 65.536 posições** (3,92% antes).
  Entradas de 992 B espalhadas por 62 MiB sugerem faltas de cache/TLB, mas o
  custo também pode estar no lock, no `PageVersion` ou no `try_get` da imagem.
  Antes de mudar o layout (índice compacto separado ou associatividade), fazer
  um histograma de IPs dentro do TryResolve com o mesmo binário da captura.
- **Causa principal do Yōtei com bindless.** `PrepareBindlessHeaps` relê e
  resolve o heap inteiro a cada consumidor (comentário "ponytail" em
  descriptors.cpp). FindSlot e ReleaseKey são os próximos no perfil.
- **A/B controlado.** As capturas de 4.096 e 65.536 posições não têm a
  mesma configuração: só a segunda tem as threads `CP recorder` e
  `Host staging copier`. Os 3 FPS são observação, não A/B.
- **Program cache.** O layout da chave subiu de 4 para 5 (e já tinha subido
  de 3 para 4 na fase 0): o primeiro run de cada jogo recompila tudo. Não
  medir FPS nesse run.

## Validação

Build Release (clang, `_Build/rf` do worktree kyty-review-fixes) de
`kyty_emulator`, `shader_recompiler_compute_tests` e `shader_cfg_tests`, sem
warnings novos. Testes rodados um por vez (limite de 12 GB), todos passando:

- memo: `texture_memo_capacity`, `texture_memo_capacity_bindless`,
  `texture_memo_revalidate`, `_verify`, `_off` e `_large`;
- F4 e fase 0: `bda_candidates_cp0`, `bda_candidates_cp1` e `bda_writes`;
- F2: `coherence_copy_tex0_read` e `coherence_copy_read_unbatched`.

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
