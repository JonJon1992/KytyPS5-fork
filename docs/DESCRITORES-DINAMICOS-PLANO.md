# Descritores dinâmicos (bindless): estado atual e plano de suporte

Data: 5 de outubro de 2026. Referência do código: `de8e3ba6` (branch `guest-sync-release-mem`).

Motivação: Ghost of Yōtei e, pelo histórico, outros jogos novos de PS5 (Wolverine, PR upstream #937;
Demon's Souls, que já usa `KYTY_SRT_VARIANT_READS` no preset) montam descritores de textura (T#),
sampler (S#) e buffer (V#) **dentro do shader**, a partir de dados em memória, com um índice que só
existe na GPU. O emulador resolve todo descritor na CPU antes do draw, então esses shaders hoje
derrubam o emulador ou são descartados.

Classificações, como em `RAY-TRACING-VIABILIDADE-VULKAN.md`:

- **Código:** comportamento confirmado na árvore citada.
- **Medido:** observado em execução nesta máquina (RX 9070 XT, RADV, Mesa 26.2.3).
- **Documentação:** contrato externo (especificação Vulkan via Context7).
- **Inferência:** conclusão técnica a partir das evidências.
- **A validar:** precisa de teste, captura ou engenharia reversa.

## 0 Estado em 6 de outubro de 2026

As seções 1 a 7 descrevem o código de 5/10 e continuam valendo como análise. Desde então o caminho
escolhido foi o **bindless com tradução na GPU** (parte da opção B), trazido do Wolverine, e não a
enumeração na CPU da opção A.

**Feito (Código):**
- **Fase 0:** `KYTY_RUNTIME_DESCRIPTOR_REPORT=1` e `tools/descriptor_report.py` (`c55c8bae`,
  `e4d0f0b7`). A primeira execução no Yōtei reportou 14 shaders descartados: 6 de imagem e 8 de
  buffer (V# calculado no shader).
- **Heap bindless** (opt-in, `KYTY_BINDLESS=1`; merge `365aa45b`): arrays de imagens por tipo de
  view, tabela de tradução chave → slot preenchida pelo host, feedback de chaves não residentes e
  samplers bindless (`KYTY_BINDLESS_SAMPLERS`, ligado por padrão quando há bindless). Usa descriptor
  indexing, então a seção 2.3 ("nada de descriptor indexing") ficou desatualizada.
- **Registros com stride** (`cbe04617`): T# em `chave × stride + imediato` (Yōtei: materiais de
  440 bytes). Recuperou 3 VS, 4 PS e 3 CS, cerca de 295 mil draws.
- **Compute com stride** só com `KYTY_BINDLESS_STRIDED_COMPUTE=1` (`589460f6`). Ligado, CSs do
  Yōtei produziram tamanhos de dispatch lixo e a GPU travou; `KYTY_DISPATCH_GROUP_LIMIT` (`b7730dc0`)
  só pula os dispatches gigantes.
- **Amostragem com comparação** (`image_sample_c`, sombras) fica fora do bindless (`58e06b15`).
- **HTile com mais de 32 camadas:** resolvido (`575cb979`). A seção 7 ainda cita o erro antigo.
- **T# r128 e S# no mesmo registro** (CS `0x34be6ffcc212383c`, esta mudança): o T# de 4 dwords
  (`IndirectImage::compact`, o host lê os dwords 4..7 como zero) e o S# em registro com stride
  (`BindlessSampler::record_stride`). Teste `TestBindlessCompactRecordWithSampler`. **A validar no
  jogo**: por ser compute, ainda depende de `KYTY_BINDLESS_STRIDED_COMPUTE=1`.

**Falta:**
1. **Causa dos dispatches lixo** com compute strided ligado. Sem isso o CS `0x34be…` e os outros
   CSs com registros continuam desligados por padrão.
2. **Comparação no bindless (prioridade baixa):** sombras (`image_sample_c`) seguem pelo caminho
   sem bindless. Pelo `58e06b15`, esses draws já renderizam por ele, porque as tabelas deles são
   enumeráveis na CPU. Só vale implementar se aparecer uma tabela de sombras com chave da GPU que o
   caminho antigo não resolva. Cuidado ao implementar: o heap resolve todas as chaves da tabela com
   o recurso de comparação, e uma tabela que misture texturas de cor e de profundidade quebraria
   nessa validação.
3. **`IMAGE_LOAD` e storage através de registro:** o PS `0x617c7166f3308810` continua descartado
   (Fase 3).
4. **V# calculado no shader** (8 dos 14 do relatório): o `IndirectBuffer` só cobre leituras raw
   x2/x3/x4 e escalares; stores e acessos formatados não.
5. **IndirectImage sem bindless:** continua com `OpSwitch` e teto de 64 imagens (Fase 1, item 4).
6. **Perguntas da seção 6** ainda abertas: 1, 2, 3, 5 e 6.

## 1 O problema no Ghost of Yōtei

**Medido.** Sem `KYTY_SRT_VARIANT_READS`, o jogo fecha no primeiro shader desse tipo
(`shader resource tracking: ... GetImageResource dword 0 is not a valid runtime value`,
`ResourceTracking.cpp`). Com `KYTY_SRT_VARIANT_READS=1`, ele roda alguns minutos (1,2–1,7 fps), mas
descarta cada dispatch/draw desses shaders. Numa execução de ~4 minutos foram **110 shaders
distintos descartados: 95 compute, 13 pixel e 2 vertex** (mensagem
`KYTY_SRT_VARIANT_READS: ... computes a descriptor at runtime ... that has no BDA path`). Com tanto
compute de fora, iluminação, sombras e pós-processamento ficam faltando.

**Código (disassembly do CS `0x34be6ffcc212383c`, o primeiro que falha).** O padrão:

```
s[36:39] = V# em SRT+32          ; faixas por tile/cluster
s[56:57] = s_buffer_load_dwordx2 s[36:39], (tile*12)+4   ; [início, fim) da lista
loop enquanto s56 < s57:
  s[12:15] = V# em SRT+96        ; lista de índices
  s60      = s_buffer_load_dword s[12:15], s56*4          ; índice do registro
  s[52:55] = V# em SRT+0         ; tabela de registros
  rec      = s60 * 0x368         ; registro de 872 bytes
  s[36:51] = s_buffer_load_dwordx16 s[52:55], rec+0
  s[58:59] = s_buffer_load_dwordx2  s[52:55], rec+128     ; flags (s_bitcmp1)
  ...                                                      ; +112, +120, +124, +168, +272, +460, +816
  s[12:19] = s_buffer_load_dwordx8  s[52:55], rec+136     ; S# (rec+136) e T# r128 (rec+152)
  image_sample_d v14, v0, T#=s[16:19], S#=s[12:15]          ; pc 0x5c8, r128=1
```

**Inferência.** É iluminação/decalque em clusters: cada registro de luz ou decalque traz a própria
textura (cookie, projeção ou sombra) **e o próprio sampler** embutidos. A tabela (`SRT+0`), a lista (`SRT+96`) e as faixas
(`SRT+32`) vêm do SRT, então a CPU sabe onde elas estão no momento do dispatch. Só o índice `s60`
vem de um loop por pixel ou thread.

Outros bloqueios do Yōtei, independentes deste plano:
- **Depth target com mais de 32 camadas:** `HTile clear tracking supports at most 32 slices`,
  `depthRenderTarget.cpp`. É o fechamento atual com `KYTY_SRT_VARIANT_READS=1`.
- **Formato 30 (`k11_11_10UNorm`):** não tem formato Vulkan direto e hoje vira textura nula.
- **Cadeias de mips 16×16 → 2×2 (RGBA16F, Standard4KB):** ficam num bloco de 4 KB com
  alinhamento de 256 bytes. Hoje são ligadas como imagens nulas (commit `2211275e`).

## 2 O que já existe

### 2.1 Leituras variantes e buffers indiretos (BDA)

**Código.**
- **Chave e cache:** `KYTY_SRT_VARIANT_READS` (`CodegenOptions.h:124`, lido em
  `CodegenOptions.cpp:63`) faz parte do fingerprint do cache (`CodegenFingerprint.cpp:85`).
- **Plano de leituras:** `BuildSrtPlan` (`SrtWalker.cpp`) transforma em leitura em tempo de execução
  toda leitura escalar cujo endereço não é um valor avaliável na CPU (phi de loop, dado produzido
  pela GPU). Ela deixa de ser um slot SRT avaliado antes do dispatch.
- **Buffers indiretos:** `TrackResources(..., indirect_scalar_buffers)` (`ResourceTracking.cpp`)
  converte um `S_BUFFER_LOAD` através de um V# calculado em `ResourceKind::IndirectBuffer`. O
  emissor (`LoadIndirectScalarBuffer`, `spirvEmitterMemory.cpp`) decodifica base, stride e
  `num_records` do V# na própria GPU e lê por BDA, com os limites do RDNA2 7.2.1.
- **Imagens e samplers não têm caminho equivalente:** `MarkUnresolved`, e o programa é descartado
  (`NoteUnresolvedDescriptor`, `ShaderRecompiler.cpp:518`). O draw cai em `renderDraw.cpp`
  ("pixel program missing").

Origem: commit `938c6cae`, feito para a travessia de BVH do Astro Bot
(`CHANGE-CATALOG.md:7289-7335`).

### 2.2 IndirectImage (tabela de T# enumerada na CPU)

**Código.**
- **Estruturas:** `DescriptorSource::IndirectImage` (`ShaderIR.h:513`) e
  `ImageResource::indirect_root/indirect_resources` (`ShaderIR.h:142`).
- **Reconhecimento:** `TryMakeIndirectImage` (`ResourceTracking.cpp:869-987`) exige três coisas:
  - **Leituras:** os 8 dwords do T# vêm de leituras escalares da mesma tabela, em offsets
    consecutivos.
  - **Offset:** tem a forma **`(chave << 5) + imediato`** (`MatchTableOffset`), ou seja, um vetor de
    T# de 32 bytes.
  - **Chave:** é um bitscan de máscara, um contador de loop `0..n` limitado ou uma "chave de
    material" lida de um buffer (`MatchMaterialOffset`, `MatchUniformizedMaterialKey`).
- **Materialização, a cada draw:** `MaterializeIndirectImage` (`ResourceMaterialization.cpp:377`)
  enumera na CPU todas as chaves possíveis (até 65536 sondagens), lê cada T#, elimina repetidos e
  cria um `ImageResource` filho por candidato. Também grava no SRT achatado um mapa ordenado
  `(chave, ordinal)`.
- **SPIR-V:** `spirvEmitterImage.cpp:1290-1390` faz uma busca binária da chave nesse mapa, depois um
  `OpSwitch` com um `OpImageSample*` por candidato. **Não há indexação dinâmica de descritores.**
- **Limites:**
  - **Contagem:** raiz mais filhos cabem em `ShaderInfo::MaxImages = 64`.
  - **Operações:** só `ImageSampleRaw` (`EXIT_IF` em `ResourceMaterialization.cpp:1496`); FMASK não.
  - **Uniformidade:** todos os candidatos precisam ter a mesma dimensão, classe numérica, mips e
    swizzle.

Origem: nmzik (`83ee040b`, `430de9bd` e `7804eef9`) e o port do PR upstream #500 (Demon's Souls).

**Por que o Yōtei não se encaixa (Código + Inferência):**
- **Offset:** é `chave × 0x368 + 152`, não `chave << 5`.
- **T#:** é de 4 dwords (`r128`), não de 8.
- **Sampler:** vem do mesmo registro (`+136`), e o IndirectImage só cobre a imagem.
- **Chave:** vem de uma lista de índices percorrida num loop por tile, não de bitscan nem de contador.
- **Candidatos:** o número de texturas distintas na tabela pode passar de 64 (**A validar**).

### 2.3 Como os descritores chegam ao Vulkan hoje

**Código.**
- **Conjunto:** um descriptor set por pipeline. São push descriptors quando cabem em
  `maxPushDescriptors`, senão sets comuns (`pipelineLayoutCache.cpp`, `descriptors.cpp`).
- **Tipos:** imagens como `eSampledImage`/`eStorageImage` em quantidade fixa por classe, e samplers
  separados (`eSampler`).
- **Recursos ausentes:** nada de descriptor indexing, `runtimeDescriptorArray`, `NonUniform`,
  partially bound, update-after-bind, `VK_EXT_descriptor_buffer` ou mutable descriptor.
  As features 1.2 pedidas estão em `RequiredVulkan12Features` (`vulkanWindow.cpp:66`).
- **Resolução de texturas, toda na CPU:** `MaterializeResources` lê os T# na memória guest. Depois,
  `ResolveTexture` → `BuildTextureDescription` → `TextureCache::FindImage` cuida de residência,
  upload, detile e metadados. Dimensão, classe e contagem de imagens entram na permutação do
  pipeline.

## 3 Recursos Vulkan disponíveis

**Documentação (Vulkan Guide e especificação, via Context7):**
- **Descriptor indexing** (núcleo do 1.2; recursos opcionais): arrays de descritores de tamanho em
  tempo de execução (`runtimeDescriptorArray`), índice não uniforme (`NonUniform` / `nonuniformEXT`),
  slots não preenchidos (`PARTIALLY_BOUND`) e atualização depois do bind (`UPDATE_AFTER_BIND`). É a
  base clássica de bindless.
- **`VK_EXT_descriptor_buffer`:** os descritores ficam em buffers comuns, escritos com
  `vkGetDescriptorEXT` e ligados com `vkCmdBindDescriptorBuffersEXT`. Isso evita pools e sets e
  permite escrever descritores em massa.
- **`VK_EXT_descriptor_heap`** (novo): heaps de recursos e de samplers acessíveis no shader como
  arrays (`layout(descriptor_heap) uniform texture2D heapTexture2D[]`), no estilo do SM 6.6 do D3D12.
  Inclui mapeamento de set/binding para offsets do heap e uma interface de "push data" que substitui
  push constants e push descriptors.
- **`VK_EXT_mutable_descriptor_type`:** um slot aceita vários tipos de descritor, o que é útil para
  um heap único de imagens sampled e storage.

**Medido (vulkaninfo, RX 9070 XT, RADV GFX1201, Mesa 26.2.3, Vulkan 1.4.354):**
- **Suporte:** `descriptorBuffer`, `descriptorHeap`, mutable descriptor, `runtimeDescriptorArray`,
  `descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`,
  `shaderSampledImageArrayNonUniformIndexing` e `shaderStorageImageArrayNonUniformIndexing`, todos
  `true`.
- **Limites:** `maxPerStageDescriptorUpdateAfterBindSampledImages` = 8 388 606. Descritor de imagem
  sampled de 32 bytes, de sampler de 16.

**A validar:** suporte e desempenho em NVIDIA e Intel no Windows. O projeto também mira Windows, e
`descriptor_heap` ainda é muito novo para ser exigido. Indexação não uniforme pode virar um loop
"waterfall" no driver, e o custo disso numa onda divergente precisa ser medido.

## 4 Opções de arquitetura

### A. Generalizar a enumeração na CPU (estender o IndirectImage)

1. **Reconhecimento:** aceitar offset `chave × stride + imediato`, com qualquer stride, e chave de
   qualquer origem em tempo de execução (inclusive de loop), desde que a **tabela** seja avaliável
   na CPU (V# vindo do SRT).
2. **Enumeração:** a CPU percorre a tabela inteira (`num_records / stride` registros), lê o T# e o
   S# de cada registro, elimina repetidos e resolve cada textura e sampler como hoje.
3. **SPIR-V:** trocar o `OpSwitch` por **indexação de array de descritores** (`NonUniform`, array por
   classe ou dimensão), com o mapa `registro → índice no array` num buffer. Isso tira o teto de 64
   imagens e o custo de um switch com centenas de casos.
4. **Cache da enumeração:** chave = endereço e tamanho da tabela + geração de escrita das páginas
   (o rastreamento de memória já existe). Sem isso, o custo por draw cresce com o tamanho da tabela.

- **A favor:** reaproveita o cache de texturas (residência, upload, detile, sincronização) e o
  fluxo por draw, sem mudar o modelo de memória. É o menor salto a partir do que existe.
- **Contra:**
  - **Custo de CPU:** cresce com o tamanho da tabela, sem o cache de enumeração.
  - **Tabelas escritas pela GPU:** a CPU lê dados velhos. É preciso detectar esses casos e cair
    para o descarte.
  - **Uniformidade:** não resolve descritores produzidos pela GPU. Candidatos de dimensão ou classe
    diferentes exigem arrays separados.

### B. Heap bindless de verdade, com tradução na GPU

1. **Heap global:** usar `descriptor_buffer` ou `descriptor_heap` com todas as imagens residentes do
   cache de texturas.
2. **Tradução no shader:** o shader lê o T# guest por BDA e consulta uma **tabela hash na GPU**,
   mantida pela CPU, de "T# normalizado (endereço, formato, dimensões, mips, swizzle) → índice no
   heap".
3. **Falhas:** um T# fora da tabela usa um descritor nulo e grava num **buffer de feedback**. A CPU
   cria a textura e ela aparece no frame seguinte, como no streaming de texturas.

- **A favor:** é geral. Cobre qualquer padrão, inclusive descritores produzidos pela GPU, e o custo
  de CPU por draw não depende da tabela.
- **Contra (grande):**
  - **Residência e ciclo de vida:** as texturas precisam ficar residentes, com ciclo de vida e
    layout conhecidos, sem `FindImage` por draw.
  - **Sincronização:** escritas guest em texturas usadas sem bind explícito precisam ser
    sincronizadas.
  - **Feedback:** a textura faltante aparece com um frame de atraso.
  - **Manutenção:** a tabela hash precisa ser mantida.
  - **Depuração:** fica muito mais difícil.

### C. Híbrido (recomendado)

Fazer A primeiro, com a infraestrutura de arrays de descritores que B também vai exigir, e deixar B
só para os padrões que A não cobrir, como tabelas escritas pela GPU. A decisão sobre B sai dos dados
da Fase 0.

## 5 Plano em fases

**Fase 0: medir antes de construir.** Criar um relatório por shader descartado (por exemplo
`KYTY_RUNTIME_DESCRIPTOR_REPORT=1`) que classifique o padrão:
- tipo de recurso (T#, S# ou V#);
- origem da tabela (SRT avaliável ou dado da GPU);
- forma do offset (`chave<<5`, `chave×stride` ou outra);
- origem da chave;
- quantos candidatos distintos a tabela tem.

Rodar no Yōtei e em outros jogos que usam `KYTY_SRT_VARIANT_READS`. Resultado esperado: a fração dos
110 shaders que a opção A cobre. *Critério:* se A cobrir a maioria, seguir com A.

**Estado (branch `desc-dyn-phase0`):** implementado o relatório. `KYTY_RUNTIME_DESCRIPTOR_REPORT=1`
imprime, uma vez por shader, dword e pc, uma linha `KYTY_RUNTIME_DESCRIPTOR` com a árvore de operações
do dword que não pôde ser resolvido na CPU (`ResourceTracking.cpp`, `GetHandle`). É diagnóstico: não
altera o código gerado nem o fingerprint do cache. `tools/descriptor_report.py <log>` agrupa as linhas por
tipo de recurso, origem da tabela, origem da chave e forma do offset, e conta shaders distintos.
Os testes de tracking passam com e sem a variável. Falta a execução no Yōtei, que o usuário roda.

**Fase 1: infraestrutura de arrays de descritores** (sem mudar o comportamento):
1. **Features:** habilitar as de descriptor indexing quando existirem (opcionais, com caminho atual
   como fallback).
2. **Layout:** binding de array em tempo de execução por classe de imagem, com `PARTIALLY_BOUND`.
3. **Emissor:** `RuntimeDescriptorArray` + `NonUniform` no SPIR-V.
4. **IndirectImage existente:** trocar o `OpSwitch` por indexação quando houver suporte.
5. **Validação:** mesmos resultados nos testes de `ResourceTrackingTests` e nos casos de GPU do
   IndirectImage, comparados com o caminho `OpSwitch`. Medir o custo por draw.

**Fase 2: tabelas com stride arbitrário e chave em tempo de execução (A):**
1. **Tracking:** `MatchTableOffset` aceita `chave × stride + imediato`. A chave pode ser qualquer valor
   em tempo de execução se a tabela vier do SRT.
2. **Enumeração:** `MaterializeIndirectImage` percorre a tabela por stride, com o cache por geração
   de escrita.
3. **Bind:** o teto de 64 é substituído pelo array da Fase 1.
4. **Tabelas escritas pela GPU:** a tabela sobreposta por escrita de GPU pendente (BDA sujo,
   dispatch anterior na mesma submissão) cai no descarte e é reportada.
5. **Teste sintético:** reproduzir o padrão do CS `0x34be6ffcc212383c` num teste de tracking e num
   caso de GPU com 3 registros e 2 texturas distintas.

**Fase 3: samplers e outros tipos.**
- **S# embutido no registro:** samplers enumerados na CPU num array de samplers. **Necessário para o
  Yōtei:** o primeiro shader já lê o S# do registro. Avaliar trazer isso para a Fase 2.
- **Imagens storage e atomics:** em array, com as mesmas regras.
- **V# dentro de registros:** já funciona pelo caminho BDA (`IndirectBuffer`). Conferir que o loop do
  Yōtei usa esse caminho.

**Fase 4 (condicional): heap com tradução na GPU (B)**, só se a Fase 0 mostrar padrões relevantes
que A não cobre. Desenhar a residência e o feedback junto com o cache de texturas.

**Validação por fase, no jogo:**
- contagem de shaders descartados no Yōtei (hoje 110 em ~4 min);
- capturas comparando com e sem a mudança;
- µs/draw do `MaterializeResources` antes e depois. Sempre com o preset U59 (ver
  `perf-runs-need-u59-preset` nas notas).

## 6 Perguntas em aberto (A validar)

1. Quantas texturas distintas a tabela de 872 bytes do Yōtei tem por frame? Isso define o tamanho
   dos arrays e o custo da enumeração.
2. Algum desses registros é escrito pela GPU (por exemplo, culling de luzes num compute anterior) ou
   todos vêm da CPU do jogo?
3. Os candidatos têm dimensões, formatos ou classes diferentes dentro da mesma tabela?
4. O PR upstream #937 (bindless do Wolverine) e o #811 ("address-backed indirect image
   descriptors") resolvem parte disso? Vale ler antes da Fase 2 para não divergir do upstream sem
   necessidade (`DIVISAO-TRAVAMENTOS.md`, `perf-research/raw-findings.md:1221`).
5. Qual o custo da indexação não uniforme no RDNA4/RADV com ondas divergentes, comparado ao
   `OpSwitch` atual?
6. Exigir descriptor indexing é aceitável para as GPUs-alvo no Windows, ou o caminho `OpSwitch`
   precisa continuar como fallback?

## 7 Como reproduzir

```sh
cd _Build/linux-clang/install
KYTY_SRT_VARIANT_READS=1 nice -n 15 ionice -c3 ./kyty_emulator <argumentos do launcher> \
    --game <pasta do Yōtei>/eboot.bin 2>&1 | tee yotei.log
grep -c "computes a descriptor at runtime" yotei.log
```

Sem `KYTY_SRT_VARIANT_READS=1` o emulador fecha no CS `0x34be6ffcc212383c`. Com ela, em 5/10 fechava
em `HTile clear tracking supports at most 32 slices`, resolvido em `575cb979`. O Yōtei pesa muito no
sistema (page faults, compilação de shaders), por isso a prioridade baixa.

Para o caminho bindless: `KYTY_BINDLESS=1`, e para os compute shaders com registros também
`KYTY_BINDLESS_STRIDED_COMPUTE=1` (ver a seção 0, item 1 de "Falta").
