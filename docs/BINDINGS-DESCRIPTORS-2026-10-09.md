# Radeon: bindings e descriptors (quinta frente) — baseline e plano

Preparado pelo Claude para o Codex, para a quinta frente de
`prompt_otimizacao_radeon_rdna_kytyps5.md` (seção 6, "Descriptors / bindings").
Base: `701751b1` mais o diff não commitado das frentes 1–4. O padrão continua
em quatro entradas, mas o patch muda busca/substituição e adiciona contadores
permanentes no cache. Custo idêntico ao código anterior não está demonstrado.

## Resumo

- **Gravar descriptors custa pouco no Thread_Gpu.** `CommitBindings` ocupa 3,8%
  inclusivo. Desse total, `CommandBuffer::PushDescriptors` responde por 0,7% e
  `CommitDescriptorSet` por 0,6%. O `vkCmdPushDescriptorSetKHR` roda no thread
  do recorder, que não é o caminho crítico.
- **O custo está em resolver os bindings de buffer:** cerca de 20% do
  Thread_Gpu, sem contar `PrepareBda`, que pertence à frente de coerência.
- **Maior subcusto isolável: 4,3% do Thread_Gpu.** Cada leitura pequena copiada
  para o stream ring busca o mapeamento guest pelo caminho lento: `m_mutex`, depois
  `std::map` e futex. O cache por thread de mapeamentos tem só 4 entradas.
- **Menor mudança proposta:** medir os motivos de miss (Etapa 0) e aumentar esse
  cache por thread com switch ao vivo (Etapa 1). A mudança fica restrita a
  `src/kernel/memoryAddressSpace.inc`, que o diff atual não toca.

## 1 Baseline medido

- **Fonte:** `perf record -t <Thread_Gpu> --call-graph fp` no Crash 4, build
  consolidado `321aa650`, com 4.235 amostras (85% de um núcleo).
- **Símbolos:** mapa `_Build/crash-run/kyty_emulator.321aa650.map`.
- **Dados e scripts:** copiados para `_Build/bindings-20261009/`: `gt5.txt`,
  `sym3.py` (self e inclusivo), `sub3.py` (filhos e folhas de uma função) e
  `callers.py` (cadeias de chamadores).

```sh
cd _Build/bindings-20261009
python3 sub3.py ../crash-run/kyty_emulator.321aa650.map gt5.txt 0 'BufferCache::ObtainBufferNow()'
python3 callers.py ../crash-run/kyty_emulator.321aa650.map gt5.txt 0 'Memory::GuestBackingStore::FindContainingUnlocked()' 4
```

| Função (inclusivo) | % do Thread_Gpu |
| --- | ---: |
| `RenderExecutor::PrepareGraphicsBindings` | 26,4 |
| ├ `RenderContext::PrepareBda` (coerência, fora desta frente) | 14,9 |
| ├ `RenderExecutor::RebindBuffers` | 7,2 |
| ├ `RenderExecutor::FindBuffers` | 2,6 |
| └ `RenderExecutor::RebindImages` | 1,0 |
| `AcquireVertexBuffersInto` (vertex buffers, mesmo `ObtainBuffer`) | 7,0 |
| `RenderExecutor::PrepareBindings` (texturas; `ResolveTexture` 2,2, `TryResolveRun` 1,4) | 5,5 |
| `RenderExecutor::CommitBindings` (self 1,8; push 0,7; set 0,6) | 3,8 |
| `BufferCache::ObtainReadBinding` (todas as origens; self 3,1; `RecordBinding` 1,0) | 12,8 |
| `BufferCache::ObtainBufferNow` | 10,3 |
| ├ `TryTransferBacking<TryReadBackingDirect>` (caminho lento) | **4,3** |
| ├ libc (memcpy da cópia no caminho rápido) | 2,2 |
| └ `BufferCache::SynchronizeBuffer` | 1,4 |
| `BufferCache::FindBuffer` → `CreateBuffer` (libdrm, em regime) | 1,3 |
| `CommandStream::Ring::WaitConsumed` (Thread_Gpu esperando o recorder) | 1,0 self |

Esses 4,3% se dividem assim:

| Folha | % do Thread_Gpu |
| --- | ---: |
| libc: lock/unlock do `m_mutex` e memcpy | 1,42 |
| `FindContainingUnlocked` (4 hints, depois `std::map::upper_bound`) | 1,18 |
| Kernel, sem símbolo (futex: `m_mutex` disputado) | 0,83 |
| Self | 0,43 |

`FindContainingUnlocked` aparece em 3,1% do Thread_Gpu, por estes chamadores:

| Chamador | % do Thread_Gpu |
| --- | ---: |
| Cópias do stream ring | 1,2 |
| `UploadCopies` da coerência | 1,0 |
| `MetadataStateHolds`, `ReadGuestForCp` e `ReadForDrawPrep` | ~0,5 |

## 2 Causa

Fluxo das leituras pequenas, `BufferCache::ObtainBufferNow` (`bufferCache.cpp`, a
partir de ~4498):

1. Uma leitura de até 16 KiB (`CACHING_PAGESIZE`), CPU-dirty e sem GPU-dirty, é
   copiada para o stream ring.
2. Com `KYTY_CP_COMMIT=all` (preset U59), a cópia passa por
   `TryReadBackingDirect` (`memoryAddressSpace.inc`, ~404).
3. `TryReadBackingDirect` consulta primeiro `t_backing_mapping_cache`
   (`GuestBackingMappingCache`, ~51–62): 4 entradas por thread, substituição FIFO,
   validade pela geração global `s_map_generation`.
4. Num miss, a leitura cai em `TryTransferBacking` (~908):
   - toma o `m_mutex`, compartilhado por todas as threads: CP, workers do
     DrawPrep e coerência;
   - consulta 4 `m_map_hints` e, se eles também falharem, chama
     `std::map::upper_bound` sobre `m_maps`;
   - só então lembra o mapeamento no cache por thread.

**Hipóteses (a validar na Etapa 0):**

- **A — capacidade.** No Thread_Gpu, as mesmas 4 entradas FIFO atendem várias
  fontes que leem de mapeamentos diferentes:
  - cópias de constantes e vertex buffers;
  - leituras do CP (`ReadGuestForCp`);
  - metadados (`MetadataStateHolds`);
  - leituras do DrawPrep.

  Isso basta para um thrash.
- **B — rotatividade da geração.** Todo map ou unmap guest incrementa a geração
  global e invalida todos os caches. Se o jogo mapeia por frame, aumentar o cache
  não resolve.

A proporção de misses no perfil é alta: o caminho lento ocupa 4,3%, contra 2,2%
do memcpy do caminho rápido. Os contadores `BackingMapCacheHits/Misses` existem,
mas só contam com o Tracy conectado ou com `KYTY_PROFILE_COUNTERS=shared`, e
nenhum log os imprime.

## 3 O que já existe (não refazer)

**Push descriptors:**
- Layout que cabe em `maxPushDescriptors`, com shadow que evita pushes iguais
  (`DescriptorPushesAvoided`).
- `KYTY_PUSH_SHADOW_FRESH_SKIP=1` no preset.
- `KYTY_DRAW_RUN_PUSH=1`: na continuação, só os descriptors por draw.

**Sets:**
- `DescriptorSetReuse`, por tick.
- `KYTY_SET_REUSE_FRESH`, ligado por padrão.
- `KYTY_RECORDER_DESCRIPTOR_SETS=1`: o recorder escreve os sets.

**Catálogo:** layouts internados (#70); dedup de uploads, shadow de push constants
e reuso de sets (#71).

**Buffers:**
- Memo por época (`KYTY_BINDING_EPOCH_MEMO`) e entre épocas (#228, #275).
- Hot memo (`KYTY_CP_BINDING_HOT_MEMO`, desligado, ao vivo).
- Prefetch do memo (`KYTY_CP_BINDING_MEMO_PREFETCH`, desligado, ao vivo).
- Prefetch em lote (`KYTY_CP_BINDING_BATCH_PREFETCH`, ligado).

**Texturas e samplers:** `TextureBindingMemo` (#69, #83, #194) e memo de sampler
de 4 vias (#264).

**Planos de binding nos workers:** `KYTY_DRAW_PREP_BINDINGS=1` no preset.

**Auditorias** (precisam de contadores):
- `KYTY_DESCRIPTOR_OFFSET_AUDIT`: mede o que offsets dinâmicos ganhariam.
- `KYTY_DESCRIPTOR_SET_REUSE_AUDIT`.

**Heap bindless** (`KYTY_BINDLESS`): é correção para o Yōtei, não performance. Ver
`DESCRITORES-DINAMICOS-PLANO.md`.

## 4 Plano

### Etapa 0 — contadores sem Tracy (diagnóstico, desligado) — **implementada no patch**

**Mudança:** `KYTY_BINDING_STATS=1` (startup) acrescenta, a cada linha
`Hot pages 10s`, uma linha com os contadores. A linha sai do Thread_Gpu, por
`BufferCache::LogBindingStats`:

```
Bindings 10s: buffers created N (X MiB), joined N, idle-freed N; GPU-thread backing map cache (E entries) hits N, misses N absent + N stale (P% hits), map generations +N
```

**Campos:**
- buffers criados (`CreateBuffer`) e o tamanho deles;
- buffers unidos (`JoinOverlap`);
- buffers aposentados por ociosidade (`m_idle_freed`);
- lookups do cache de mapeamentos do Thread_Gpu: hits e misses por motivo,
  ausente ou só registros de geração antiga;
- incrementos da geração de mapa (maps/unmaps guest).

**Como contam:** os contadores do cache ficam no próprio TLS do cache
(`GuestBackingMappingCache`). Cada um é um incremento comum, sem atomic, e
`Memory::BackingMapCacheThreadStats()` lê os da thread que chama. Por isso a linha
cobre só o Thread_Gpu, o caminho crítico.

`absent` inclui cache frio, consultas sem mapping elegível e ranges que atravessam
mappings; não prova falta de capacidade. Um hit conta a entrada encontrada,
mesmo se a revalidação após a cópia falhar e exigir fallback. Comparar deltas
após aquecimento e a variação de gerações para interpretar o A/B.

**Não cobre:** pushes, sets e misses dos `m_map_hints`. Se fizerem falta, entram
depois.

**Critério:**
- misses do tipo A dominam: seguir para a Etapa 1;
- misses do tipo B dominam: a Etapa 1 não resolve. Nesse caso, avaliar geração
  por faixa ou consulta lock-free a um snapshot ordenado de `m_maps`.

### Etapa 1 — cache por thread de mapeamentos maior (menor mudança) — **implementada no patch**

**Mudança:**
- `GuestBackingMappingCache` passa a ter capacidade para 16 registros.
- As entradas ativas são definidas pelo `Live::Switch`
  `KYTY_BACKING_MAP_CACHE_ENTRIES=1..16`. O **padrão é 4**, o mesmo número de
  anterior. Isso preserva a capacidade padrão, não a política ou seu custo.
- O lookup tenta primeiro o registro do último hit e depois os demais.
- A inserção substitui, nesta ordem: o registro do mesmo mapeamento, o primeiro
  registro velho e, por fim, uma posição round-robin (não LRU nem idade exata).

**Arquivo:** `src/kernel/memoryAddressSpace.inc`. Os três leitores
(`TryReadCachedMapping`, `TryReadBackingDirect` e `TryInspectBacking`) agora
usam o helper `FindCachedMapping`, que também conta hits e misses.

**Microbenchmark** (temporário, fora do patch; leitura de 64 B por
`TryReadBackingDirect`, 2 milhões de leituras, um núcleo fixo, duas rodadas
iguais). Os números completos estão em `_Build/bindings-20261009/bench-map-cache.txt`.

| Mapeamentos lidos alternadamente | 4 registros | 16 registros |
| ---: | ---: | ---: |
| 1 | 5,2 ns | 5,2–5,4 ns |
| 4 | 6,9 ns | 6,9–7,1 ns |
| 6 | 35,0 ns | 7,2–7,4 ns |
| 8 | 35,1 ns | 7,3–7,5 ns |
| 8 (leituras de 1 KiB) | 44,9–46,0 ns | 14,3–14,4 ns |

No microbenchmark o miss custa cerca de 28 ns, com mutex sem disputa e poucos
mapeamentos na árvore. No jogo, o perfil mostra árvore maior e futex
(`m_mutex` disputado), então o miss tende a custar mais. Dentro da capacidade,
16 registros não pioram o hit.

**Por que é seguro:** o cache guarda registros de mapeamento, não bytes, e cada uso
já é validado por geração e faixa, como hoje. Mudam apenas o número de registros
e a ordem da busca.

**Benefício em Radeon:**
- O jogo é limitado pelo Thread_Gpu: a GPU fica ociosa esperando a CPU.
- Menos CPU por draw nesse thread aumenta a ocupação da RX 9070 XT.
- O ganho não é específico do driver.
- Menos disputa no `m_mutex` também ajuda os workers do DrawPrep.

**Expectativa:** o caminho lento cai de 4,3% para cerca de 1%. O memcpy permanece.
Seriam ~3% do Thread_Gpu, perto de 1 ms por frame a 25 fps. É uma estimativa, que
precisa do A/B.

**Risco:** baixo. O custo de varrer 16 entradas de 40 bytes (640 B em TLS) é
mitigado pela verificação do último hit primeiro.

**Testes** (passaram no worktree): `virtual_memory_allocation` completo, 60 casos.

O novo `TestBackingMapCacheCapacity` lê alternadamente 8 mapeamentos e verifica:
- com 4 registros, todo lookup é miss de capacidade;
- com 16 registros, todo lookup é hit depois da primeira passada;
- depois de um unmap, o primeiro lookup é miss de geração velha, seguido de bytes
  corretos e de um hit;
- o range desmapeado não é lido;
- sem a variável, voltam os 4 registros.

### Etapa 2 — A/B sem código

`KYTY_CP_BINDING_HOT_MEMO=1` e `KYTY_CP_BINDING_MEMO_PREFETCH=1` já são switches
ao vivo, mas nunca passaram por A/B no Crash 4 com o build atual. Basta
alternar pelo `live.env`.

### Etapa 3 — `CreateBuffer` em regime (1,3%)

**Evidência:** `FindBuffer` cria buffers durante o gameplay (libdrm na pilha): 0,8%
em draws e 0,6% em dispatches.

**Diagnóstico:** com o contador da Etapa 0, descobrir se as faixas crescem
(`ResolveOverlaps`, junção e stream leap) ou se a coleta de VRAM
(`KYTY_VRAM_IDLE_FRAMES`) destrói e recria buffers. Propor mudança só depois.

### Etapa 4 — micro-otimização de `CommitBindings` (self 1,8%)

**Mudança:** pré-calcular por programa o template das writes (`dstBinding`, tipo,
contagem, classe), invariante entre draws. Assim saem do caminho por draw:
- o recálculo por binding;
- o `m_image_occurrences.assign`;
- os `.at()`.

Só depois das etapas 1 a 3.

### Etapa 5 (condicional) — tabelas por draw fora dos descriptors

**Problema:** `FlattenedSrt` e `ShaderData` vão para um offset novo do stream ring
a cada draw, então todo push difere do anterior. O comentário em
`descriptors.cpp`, perto de `KYTY_PUSH_SHADOW_FRESH_SKIP`, registra cerca de
4.000 pushes por flip, todos misses.

**Restrição:** offsets dinâmicos são proibidos em layouts de push descriptor (o
próprio código registra isso em `DescriptorOffsetAudit`).

**Alternativa:** passar o endereço da tabela por push constant (BDA). Isso exige
mudança no recompilador e no fingerprint, o que recompila todos os shaders.

**Quando vale:** o ganho cai no thread do recorder, não no Thread_Gpu. Só faz
sentido se o recorder virar gargalo; hoje `WaitConsumed` é 1,0%. Medir antes com
`KYTY_DESCRIPTOR_OFFSET_AUDIT=1`.

### Fora de escopo agora

Migrar para `VK_EXT_descriptor_buffer` ou `descriptor_heap`. É uma mudança
grande, sem gargalo medido que a justifique no Thread_Gpu.

## 5 Método de A/B

- **Preset e perfil:** U59 mais o perfil do Crash 4. Live file em
  `_Build/crash-run/live.env`.
- **Janelas:** 5 s, ABAB ×12. A cena oscila ±25% em 20–60 s, então janelas de
  20 s não resolvem efeitos de ~10%.
- **Métricas:**
  - FPS e tempo entre flips;
  - CPU do Thread_Gpu em ms/frame (`/proc`);
  - perf `-t` do Thread_Gpu nos dois braços, com a parcela de
    `TryTransferBacking<TryReadBackingDirect>` e `FindContainingUnlocked`;
  - linha `Bindings 10s`.
- **Impacto visual esperado:** nenhum, porque os bytes lidos são os mesmos.
  Mesmo assim, comparar capturas.

## 6 Coordenação

- **Patch das etapas 0 e 1:**
  `_Build/bindings-20261009/0001-backing-map-cache-and-binding-stats.patch`.
  Sem commit. `git apply --check` passa sobre a árvore principal com o diff
  atual do Codex.
- **Arquivos tocados:** `src/kernel/memoryAddressSpace.inc`, `memory.cpp`,
  `memory.h`, `renderer/cache/bufferCache.cpp`, `bufferCache.h` e
  `tests/VirtualMemoryAllocationTests.cpp`. Nenhum deles está no diff do Codex.
- **Worktree:** `/home/jonathanbraga/kyty-bindings-wt`, branch
  `bindings-backing-cache`.
  - Os submódulos são links para a árvore principal.
  - Build próprio em `_Build/linux-clang` dentro do worktree, com ccache, `nice`
    e `-j4`.
  - Nada foi instalado e nenhum ninja rodou na árvore principal.
- **Mensagem para o Codex:** `_Build/bindings-20261009/claude-codex-message.txt`.

Comandos para o A/B ao vivo, depois de aplicar o patch e instalar:

```sh
# start.sh / preset: KYTY_BINDING_STATS=1 (startup)
# live.env: alternar a cada janela
KYTY_BACKING_MAP_CACHE_ENTRIES=4
KYTY_BACKING_MAP_CACHE_ENTRIES=16
```

## Integração pelo Codex

O patch `0001-backing-map-cache-and-binding-stats.patch` foi revisado e aplicado
à árvore principal, sem conflitos, junto das frentes anteriores. A revisão
confirmou preservação de owner, geração, bounds e revalidação após leitura,
com fallback protegido por mutex. Foram corrigidas as descrições de `absent`,
substituição round-robin e impacto do padrão de quatro entradas.

A build consolidada do emulador e dos testes passou. O CTest
`virtual_memory_allocation` completo passou, incluindo `BackingMapCacheCapacity`
(4→16 entradas, hits/misses e invalidação por unmap). Os três CTests da frente
de barriers também passaram na mesma build, com zero erros de sincronização
Vulkan e readbacks corretos. Smoke `kyty_emulator --help` passou.

Evidência: `_Build/barriers-20261009/integrated-tests.log`,
`build-integrated.log` e `artifacts.json`. Executável congelado em
`_Build/barriers-20261009/kyty_emulator.tested`. Não houve instalação no atalho
do jogo nem alteração do preset. Padrão do cache: 4 entradas; logging desligado.
O A/B 4 versus 16 no Crash 4 continua pendente a pedido do usuário. Os números
de microbenchmark acima são os fornecidos pelo Claude, não uma medição nova
de FPS ou frame time da build integrada.
