# Auditoria: shaders e pipeline cache (travamentos no gameplay)

**Data:** 2026-10-24. **Escopo:** caminho shader do PS5 → SPIR-V → pipeline Vulkan → draw, no worktree
`WolverinePort` (branch `wolverine-port`). Relatório só de leitura: nenhuma otimização aplicada com base nele
sem medição. Complementa [ANALISE-PRECOMPILACAO-PIPELINES-VULKAN.md](ANALISE-PRECOMPILACAO-PIPELINES-VULKAN.md),
que pediu exatamente isto: separar as causas dos misses antes de pré-aquecer qualquer coisa.

**O que este repo faz:** é o KytyPS5 (base Frank) adaptado para rodar Marvel's Wolverine no PC. Ele traduz os
shaders do PS5 (programas da GPU, em código AMD) para SPIR-V (o formato de shader do Vulkan) e monta *pipelines*
Vulkan (shader + estado de desenho, compilados pelo driver da AMD) na hora em que o jogo desenha.
**Carga assumida:** uma pessoa jogando sessões normais pelo `Play.bat`, RX 9070 XT, driver 26.10.1. O que importa
é o tempo de cada quadro.

**Hipótese descartada:** os travamentos repetidos **não** vêm do cache perder as compilações otimizadas. No
`[pipeline-miss]`, de 23 misses de pares VS/PS já vistos, 0 tinham o mesmo SPIR-V: o shader mudou, não o cache.
A build das 15:40 (`KYTY_UNOPTIMIZED_CACHE_PROBE`) nunca foi instalada e foi revertida (achado 7).

## Como funciona hoje

1. O jogo manda um draw/dispatch. A Thread_Gpu (thread que executa os comandos da GPU do PS5) segura o mutex do
   renderer (`RenderContext::m_mutex`) durante o draw inteiro.
2. **Tradução** (`TranslateProgram`): só draws com teste de profundidade traduzem em workers
   (`pipelineCache.cpp:3948`). Compute, draws sem profundidade e alvos pequenos traduzem na própria Thread_Gpu
   (`pipelineCache.cpp:2707`).
3. **Especialização:** o tipo de cada textura e buffer é lido do descritor no momento do draw e gravado no SPIR-V
   (`ResourceMaterialization.cpp:689-716`). Cada combinação vira uma *permutação* (outra versão do mesmo shader).
4. **Emissão SPIR-V + `vkCreateShaderModule`:** sempre na Thread_Gpu, com o mutex (`pipelineCache.cpp:2760-2769`).
5. **Pipeline:** primeiro uma sonda no cache do driver ("me dá se já estiver pronto, senão falha na hora",
   `FAIL_ON_PIPELINE_COMPILE_REQUIRED`). Se faltar: um worker compila sem otimização; a Thread_Gpu espera
   (`[pipeline-wait]`) quando o draw não pode ser pulado; depois a versão otimizada compila quando os workers ficam
   ociosos.
6. **Disco:** `_ShaderCache` guarda tradução + SPIR-V por permutação; `_PipelineCache\PPSA03671.bin` guarda o
   cache do driver (a cada 120 s compilando e na saída).
7. O present (`swapchain.cpp:1095`, `:1111`) pega o mesmo mutex do renderer.

## Evidências (3 sessões normais + perfis)

| Medida | Valor |
|---|---|
| Sessão 24/10 15:29 | 147 s; 9 travamentos ≥3 s = 39 s travado; 29 esperas = 26,4 s (24,9 s compilando, 1,6 s na fila) |
| 133 esperas, 3 sessões | p50 613 ms · p95 2.034 ms · p99 3.162 ms; fila somou só 7,7 s → **não falta thread** |
| Por motivo | alvo pequeno 86 / 77 s (65 com o VS `0x4efbcf79c36d8adf`) · escreve memória 27 / 22 s · layer 4 / 1,7 s |
| Compilação × tamanho SPIR-V (mediana) | <40 mil palavras: 146 ms · 70–100 mil: 547 ms · 150 mil+: 1.837 ms |
| `_ShaderCache` | 1.259 shaders, 2.016 permutações; dos 208 com mais de uma, **123 diferem só no tipo de imagem** (+21 tipo de imagem e outro campo) |
| Campos que variam entre irmãos | dimension 113 · push_data_cursor 48 · packed_stride 41 · zero_stride_oob 39 · numeric_class 39 |
| 524 misses | **430** com um VS cujo SPIR-V já estava em outro pipeline da mesma sessão; 36 com PS repetido |
| VS `0x4efbcf…` | 72.867 palavras, 1 função, 687 blocos; Bitcast 20,5 % + Select 15,3 % das palavras |
| Perfis | 00:24: Thread_Gpu 42 % em `PipelineCompiler::Wait`. 15:29 (`prof_60`): fence 11,9 %, `DrawPrepPixelState` 26,6 %, `DispatchDirect` 24,9 % |
| Driver | suporta GPL + fast link, shader object, pipeline binary, module identifier; nenhum usado no `src` |

Scripts em [benchmarks/](benchmarks/): `perm_census.py` (+ `decode_spec.py`), `spirv_histogram.py`,
`resumo_rodada.py`.

## Achados

### Deve corrigir

1. **O mesmo shader vira SPIR-V novo quando a textura muda de tipo** (`ResourceMaterialization.cpp:689-716`)
   - **O que é:** ao traduzir, o recompilador lê o tipo da imagem (2D ou array 2D; float ou uint) do descritor
     que o jogo passou naquele draw e grava isso no SPIR-V.
   - **Problema:** o jogo usa o mesmo shader com textura 2D num draw e array 2D noutro. Cada variação gera SPIR-V
     novo, pipeline novo e 0,6–3 s de compilação. Ex.: PS `2209aa…` (uint vs float no slot 11), PS `204fcf…`
     (array vs 2D no slot 1). São 123 dos 208 shaders com mais de uma versão.
   - **Correção:** declarar toda imagem 2D amostrada como array (`OpTypeImage Dim2D Arrayed=1`) e ligar sempre
     uma view `2D_ARRAY` (uma textura 2D é um array de 1 camada; o Vulkan permite essa view). Instrução sem
     camada passa camada 0. Junta dimensão 3 e 5. Atrás de uma chave, desligada até ser ligada no `Play.bat`.
     uint vs float (39 shaders) fica para depois de medir.
   - **Se pularmos:** cada área nova traz combinações novas e novos travamentos de 1–3 s, em toda sessão.

2. **Muitos pipelines novos incluem um VS já usado** (`shaders.cpp:376-385`, caminho de miss no
   `pipelineCache.cpp`)
   - **O que é:** o caminho monolítico entrega VS, PS e estado juntos ao driver. Um miss da pipeline não prova,
     isoladamente, que o driver recompilou integralmente os dois estágios; o reuso interno precisa ser medido.
   - **Problema:** 430 dos 524 misses tinham um VS já compilado na mesma sessão. O VS `0x4efbcf…` (73 mil
     palavras) aparece em 65 esperas; esses draws de alvo pequeno não podem ser pulados (parecem gravar páginas
     de textura; pular deixa buracos permanentes), então a Thread_Gpu espera.
   - **Correção:** `VK_EXT_graphics_pipeline_library` (GPL): VS e PS compilam separados, como *bibliotecas*, em
     workers; no draw só se ligam (fast link); a versão otimizada (link-time optimization) compila depois, em
     segundo plano, e substitui. **Layout:** o GPL permite fornecer o mesmo layout completo às bibliotecas
     pré-raster e fragment, sem `INDEPENDENT_SETS`. Separar sets por estágio é opcional. O cache das bibliotecas
     ainda precisa respeitar layouts compatíveis, bindings e push constants; medir quanto o layout combinado
     atual limita o reuso antes de refatorar `FillSetLayouts` e os push descriptors.
   - **Se pularmos:** todo pipeline novo com shader grande trava 0,5–3 s, mesmo quando só o PS é novo.

### Deveria corrigir

3. **A compilação cresce com o tamanho do SPIR-V, e ninguém mediu se dá para encolher** (`backend/spirv/*`;
   SPIRV-Tools só valida, `pipelineCache.cpp:951`)
   - **O que é:** o SPIR-V sai sem otimização. SPIRV-Tools já está no projeto, só para validar e desmontar.
   - **Problema:** o tempo vai de 146 ms a 1,8 s conforme o tamanho. No VS `0x4efbcf…`, 36 % das palavras são
     Bitcast e Select (registradores do GCN como inteiro, convertidos a cada uso). Não se sabe se o compilador
     da AMD já descarta isso de graça.
   - **Correção:** medir antes (B1). Só se a compilação cair ≥30 %: `spirv-opt` no worker de tradução, resultado
     guardado no `_ShaderCache`.
   - **Se pularmos:** shaders grandes continuam custando 1–2 s cada, mesmo com GPL.

4. **Tradução, emissão e criação do módulo rodam na Thread_Gpu segurando o mutex que o present usa**
   (`pipelineCache.cpp:2707`, `:2760-2769`; `swapchain.cpp:1095`)
   - **O que é:** para compute, draws sem profundidade e alvos pequenos, `TranslateProgram`,
     `CompileTranslated` e `vkCreateShaderModule` rodam dentro do draw, com o mutex. A leitura do `_ShaderCache`
     também é síncrona (`KYTY_SHADER_READ_THREADS=0`).
   - **Problema:** enquanto um compute grande traduz, o present fica parado no mesmo mutex; nem o quadro anterior
     é reapresentado. Custo desconhecido: esses tempos iam para o `LOGF`, que está silencioso.
   - **Correção:** medir (achado 5). Se pesar: tradução e emissão nos workers que já existem, espera em fatias
     como o `KYTY_PIPELINE_WAIT_SUSPEND`. Esperar sempre, nunca pular.
   - **Se pularmos:** cada compute ou HUD novo soma tempo de tradução ao travamento.

5. **Não dá para medir P95/P99 de frame time** (`fase1\Play.bat`, `fps.csv`)
   - **O que é:** o fps vem do título da janela, 1 vez por segundo; `[pipeline-wait]` não tem horário; tempos
     de tradução saem pelo `LOGF` desligado.
   - **Problema:** um travamento de 300 ms dentro de um segundo vira "fps 20" e some. Não dá para provar melhora
     no P95/P99, o critério de sucesso.
   - **Correção:** uma linha por present num CSV; horário nas linhas `[pipeline-wait]`; traduções lentas no log
     que aparece; um script que resume cada pasta de `rodadas\`. Nada muda no jogo.
   - **Se pularmos:** cada fase vira opinião.

6. **Salvar o cache do driver não é atômico** (`common/file.cpp:586-597`, `pipelineCache.cpp:3742-3758`)
   - **O que é:** o cache é escrito num `.tmp` e renomeado; `RenameFile` apagava o antigo e só depois chamava
     `MoveFileW`.
   - **Problema:** se o processo morrer entre os dois passos, ou o antivírus segurar o arquivo, sobra só o
     `.tmp`, que nunca é lido. A sessão seguinte recompila tudo.
   - **Correção:** substituir de uma vez (`MoveFileExW` com `MOVEFILE_REPLACE_EXISTING`; `rename` no Linux) e
     tirar o `DeleteFile`.
   - **Se pularmos:** raro, mas custa uma sessão inteira de travamentos.

7. **Segunda sonda do cache, nunca instalada** (`pipelineCache.cpp`, `pipelineCache.h`, `shaders.cpp`)
   - **O que é:** a build das 15:40 fazia, a cada miss, uma segunda sonda pela versão não otimizada.
   - **Problema:** nasceu da hipótese errada; não acharia nada e somava uma chamada ao driver por miss.
   - **Correção:** reverter (`CacheOnlyUnoptimized`, `KYTY_UNOPTIMIZED_CACHE_PROBE`, a segunda sonda).
   - **Se pularmos:** código morto com comentário enganoso.

## Arquitetura proposta: o mínimo que muda

Ficam: workers, sonda no cache do driver, `_ShaderCache`, compilação não otimizada + otimizada depois. Mudam três
coisas:

1. **O SPIR-V depende só do código do jogo** e dos campos que mudam o código de verdade (achado 1).
2. **Pipeline em partes:** bibliotecas de VS e PS no mesmo `VkPipelineCache`, fast link no draw, versão otimizada
   em segundo plano (achado 2).
3. **A Thread_Gpu só espera, não trabalha** (achado 4).

C++: seguir o padrão que já existe (jobs em `shared_ptr`, `Common::LockGuard`, destroy por dono como no
warm-up). Nenhuma camada nova.

O que **não** usar:
- `VK_KHR_pipeline_binary`: não combina com `VkPipelineCache` nem com a sonda.
- `VK_EXT_shader_object`: sem modo não otimizado; exige a mesma separação de sets que o GPL.
- Mais threads: a fila é só 7,7 s do total.
- `VK_EXT_shader_module_identifier`: só economiza ler o SPIR-V, não compilar.
- Uber-shader: não serve para código GCN recompilado.

## Plano de migração

| Fase | O que entra | Critério para seguir | Risco |
|---|---|---|---|
| 0. Medir | achados 5, 6, 7 | 2–3 sessões normais com o CSV de frame time | nenhum: só log |
| 1. Menos permutações | achado 1, chave desligada | `perm_census.py`: ~0 irmãos que diferem só em dimensão; sem diferença visual | views `2D_ARRAY` a mais no cache de texturas; erro de tipo de view (`--vulkan-validation`) |
| 2. SPIR-V menor | achado 3, só se B1 der ≥30 % | **descartada**: B1 deu −2 % (ver Andamento) | — |
| 3. GPL | bibliotecas com layout completo compatível, fast link, LTO em segundo plano | p95 das esperas < 100 ms | medir reuso com o layout combinado; separar sets somente se necessário; pipeline ligado pode ser mais lento até o otimizado chegar |
| 4. Thread_Gpu livre | achado 4, se a fase 0 mostrar custo | nenhuma tradução > 50 ms na Thread_Gpu | compute nunca pode ser pulado |

Cada fase é uma build com backup do exe anterior e a chave nova desligada até ser ligada no `Play.bat`.

## Benchmarks reproduzíveis

- **B1 (offline, sem jogo):** o SPIR-V das pipelines que as sessões esperaram, tirado do cache
  (`wait_shaders.py <cache> <saída> <kyty_log.txt>...`), variantes do `spirv-opt` (`opt_variants.py`), e o tempo
  de criação no driver de cada uma (`pipe_bench <saída> <repetições> uo -,O,L`; compilar com
  `clang-cl /O2 /std:c++20 /EHsc /I 3rdparty/Vulkan-Headers/include docs/benchmarks/pipe_bench.cpp`). O
  `spirv-opt` sai do submódulo `3rdparty/SPIRV-Tools` (configurar só ele, com
  `-DSPIRV-Headers_SOURCE_DIR=3rdparty/SPIRV-Headers`, alvo `spirv-opt`).
- **B2 (cache em disco):** `perm_census.py` sobre `_ShaderCache\PPSA03671`. Determinístico; mede a fase 1 sem
  jogar.
- **B3 (sessões normais):** `python -I docs/benchmarks/resumo_rodada.py <pasta>` por pasta de `rodadas\`:
  P50/P95/P99/máximo do frame time, quadros > 50 ms, segundos travados, número e soma de `[pipeline-wait]`,
  traduções lentas, e quanto dos travamentos foi espera de compilação.

## Andamento

- **Fase 0 (24/10, build 16:14 instalada em `fase1`; backup `kyty_emulator.1528.exe`):**
  - achado 7: segunda sonda revertida;
  - achado 6: `RenameFile` substitui o destino de uma vez (`MoveFileExW` + `MOVEFILE_REPLACE_EXISTING`;
    `rename` no Linux);
  - achado 5: `KYTY_FRAME_TIMES=<arquivo>` (uma linha por quadro apresentado, ligado no `Play.bat` como
    `frames.csv`, arquivado em `rodadas\`); `at=` (segundos do steady clock) nas linhas `[pipeline-wait]`;
    linha nova `[shader-compile]` para tradução + emissão + módulo de 50 ms ou mais, dizendo se a tradução
    foi na própria thread (`here`) ou num worker.
  - Falta: 2–3 sessões normais para fechar a linha de base antes da fase 1.
- **Primeira leitura da linha de base (sessão de 24/10 16:15, parcial, 583 s):** p50 50 ms, p95 108 ms,
  p99 200 ms, máximo 7,6 s; 22 travamentos ≥1 s = 62,6 s, dos quais **46,4 s** batem com esperas de pipeline
  ou tradução. `[pipeline-wait]` 33 = 42,8 s. `[shader-compile]` 134 ≥50 ms = **20,4 s na Thread_Gpu**, só
  4,3 s traduzindo ali: os outros 16 s são `CompileProgram` (passes + emissão SPIR-V) e `vkCreateShaderModule`
  de shaders já traduzidos num worker. PS de 210–277 mil palavras custam 300–440 ms cada. Isso sobe o
  achado 4 (fase 4) de prioridade: `CompileProgram` também precisa sair da Thread_Gpu.
- **Linha de base 1 completa (sessão 24/10 16:15–16:26, 637 s, saída normal, caches salvos):** 18,8 fps de
  média; p50 50 ms, p95 108 ms, p99 184 ms, máximo 7,6 s; 22 travamentos ≥1 s = 62,6 s (8 de ≥3 s); 46,4 s
  cobertos por espera/tradução; os ~16 s restantes são carregamento no começo (t = 0, 41 e 55 s). Quase todo
  travamento de gameplay é o VS `0x4efbcf…` com um PS novo de 208–300 mil palavras em alvo pequeno
  (pipelines de 286–314 mil palavras, 1–3,7 s cada).
  - As 34 esperas (43,2 s), pelo motivo do pipeline novo: **PS nunca visto 21 (25,3 s)**; **permutação que a
    fase 1 evita 6 (13,2 s)**; permutação nova não evitável 4 (3,6 s); PS conhecido com VS/estado novo 3 (1,0 s).
  - Permutações novas na sessão: 270, das quais **146 a fase 1 evitaria**. Ex.: PS `6323df…` esperou 4
    vezes (1,0–2,6 s) por `image[3].dimension` 3 vs 5.
  - O PS `e089b6…` (213 mil palavras): Bitcast 22,7 % + Select 19,3 % das palavras, como o VS.
  - O VS `0x4efbcf…` tem ~20 permutações entre sessões (formato/stride de vertex buffer e
    `push_data_cursor` 0–29). O `push_data_cursor` do VS muda com o resto do pipeline, o que acopla o VS ao PS:
    verificar antes do GPL (fase 3), senão a biblioteca de VS não é reaproveitada.
- **Fase 1 instalada (24/10, build 16:30 em `fase1`, backup `kyty_emulator.1614.exe`; `KYTY_ARRAYED_IMAGES=1`
  no `Play.bat`).** A primeira sessão retraduz tudo (`SourceVersion` novo); medir a partir da segunda. Os
  registros antigos do `_ShaderCache` ficam no disco (a versão faz parte da chave), então voltar ao exe 1614
  reaproveita o cache antigo.
- **Primeiras sessões com a fase 1 (24/10):** fria 16:32–16:36 (237 s) e quente 16:36–16:39 (173 s), ambas com
  saída normal, sem erro de view/especialização no log. Os primeiros ~60 s de toda sessão são menu e
  carregamento (p50 8,3 ms); comparando a mesma janela de gameplay, 60–173 s:

  | Sessão | Quadros | p50 | p95 | p99 | Travado ≥1 s |
  |---|---|---|---|---|---|
  | Linha de base 16:15 | 777 | 99,7 ms | 208 ms | 1.134 ms | 8 (25,2 s) |
  | Fase 1 fria 16:32 | 1.420 | 50,3 ms | 108 ms | 533 ms | 6 (22,0 s) |
  | Fase 1 quente 16:36 | 2.895 | 25,2 ms | 66,9 ms | 116,9 ms | 2 (6,7 s) |

  O caminho jogado e o cache mais quente também contam; a parte que é só da fase 1 aparece no cache:
  com a chave (key hash agora no offset 2155), **1,13 permutação por shader (antes 1,64)**, e shaders cujas
  versões diferem na dimensão caíram de 163 para 1; `avoidable_perms.py` acha 0 evitáveis. As 6 esperas da
  sessão quente (7,0 s) foram todas de PS que existiam antes da fase 1 e não apareceram na sessão fria: é a
  recompilação única da migração, que some conforme cada área é revisitada uma vez.
  Sobram como fonte de versões: `numeric_class` (30 shaders), `push_data_cursor` (26), `zero_stride_oob` (19),
  `packed_stride` (16).
- **Fase 1 (detalhes):** `KYTY_ARRAYED_IMAGES=1` em `ResourceMaterialization.cpp`
  (`DescriptorDimension` + caso de textura nula), só para imagens amostradas; teste novo em
  `tests/ResourceMaterializationTests.cpp` (passa). Estimativa no cache atual (`merge_estimate.py`):
  2.267 → 1.997 permutações (−270, 244 shaders). Muda o `SourceVersion` (0x88754a605febcb2a): a primeira
  sessão com ela retraduz tudo. Instalar só depois da linha de base, já com a chave ligada.
  - `resource_materialization_tests` já falhava antes desta mudança ("dirty vector descriptor input fell
    back to an ordinary memory read", igual no HEAD); o caso novo roda primeiro para não ser escondido.
- **B1 / fase 2 (25/10, offline, RX 9070 XT, driver 26.10.1): `spirv-opt` não compensa; fase 2 descartada.**
  Em vez do RGA, um programa que cria os pipelines como o emulador cria (`docs/benchmarks/pipe_bench.cpp`:
  mesmo layout de descritores, set bindless, push de 128 bytes, tamanho de onda do guest, sem cache) mede
  as **86 pipelines das esperas** das sessões 16:15, 16:32 e 16:36 (`wait_shaders.py` tira o SPIR-V delas
  do `_ShaderCache`). Cada build leva uma constante única não usada, senão o cache interno do driver responde
  (0,05 ms na repetição). Os tempos batem com o jogo: 57,3 s no banco contra 70,5 s de `compiling=` no
  jogo, que divide a CPU com o resto.

  | SPIR-V | Palavras | Driver, sem otimização | Driver, otimizado | Custo do `spirv-opt` |
  |---|---|---|---|---|
  | como gravado | 9,95 M | 57,3 s | 57,5 s | — |
  | `spirv-opt -O` | −13,5 % | −1,8 % | −2,2 % | 54,7 s |
  | passes leves (`ccp`, `simplify`, `adce`, `redundancy`, `dead-branches`, `merge-blocks`) | −13,7 % | −2,0 % | −2,1 % | 29,8 s |

  O ruído entre repetições é 1,8 % (mediana). Nenhum tamanho de shader passa de −4 %. O driver já tira
  sozinho o que o `spirv-opt` tira; o custo dele está em outra coisa. Rodar o `spirv-opt` custaria quase o
  mesmo que a compilação que ele economiza.
- **Achados novos do B1 (medidos):**
  - `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` **não acelera nada neste driver** (57,3 s contra 57,5 s;
    pipeline a pipeline igual). A build "rápida" dos pipelines que o jogo espera (`KYTY_URGENT_FAST_COMPILE`,
    `KYTY_COMPUTE_FAST_COMPILE`) só dobra o trabalho: a build otimizada que vem depois para o cache recompila
    do zero (1.570 ms e depois 1.586 ms no mesmo pipeline). Com as duas chaves em `0`, cada pipeline novo
    compila uma vez, otimizado, no mesmo tempo da espera de hoje, e os workers ficam livres mais cedo.
  - O driver guarda pipelines e módulos em disco entre execuções: num segundo processo, o pipeline de 314
    mil palavras sai em 0,3 ms (antes 1.570 ms) e o módulo em 1,9 ms (antes 102 ms). Por isso a sessão
    quente quase não espera, e o módulo de um shader do cache em disco não pesa na Thread_Gpu.
  - `vkCreateShaderModule` de SPIR-V novo custa ~0,3 ms por mil palavras (20–100 ms nos grandes): entra na
    fase 4.
- **Onde ficam os 10 ms para shader grande (25/10):** a compilação no driver de um pipeline de 100–300 mil
  palavras leva 0,4–2 s e nada no SPIR-V a encurta (B1). Na sessão fria da fase 1, 41 das 49 esperas
  (21,9 de 26,7 s) foram de draws que escrevem na memória, que não podem ser pulados (já deram device lost).
  Então, no primeiro encontro com um shader, abaixo de 10 ms só é possível para o trabalho de CPU na
  Thread_Gpu (fase 4) e para draws que podem ser pulados ou suspensos; a espera do driver nos draws que
  escrevem memória só some com o pipeline compilado antes (sessão seguinte: cache do driver).
- **Fase 4 (25/10, código pronto, chave `KYTY_SHADER_WORKERS=1`, desligada por padrão):** o resto da
  compilação de um shader novo vai para os workers de tradução: plano de recursos, `CompileProgram`,
  registro do cache em disco e `vkCreateShaderModule`. A Thread_Gpu só escolhe a especialização (lê memória
  do guest) e depois pega o módulo pronto. Draws 3D são pulados enquanto isso, como já eram na tradução; os
  outros draws e os dispatches (compute agora também) suspendem o pacote e esperam em fatias de 4 ms, nunca
  pulados (precisa de `KYTY_PIPELINE_WAIT_SUSPEND=1`, já no `Play.bat`); sem suspensão possível, compila na
  Thread_Gpu como antes. Uma permutação que chega para outra especialização entra no shader e a busca segue.
  `[shader-compile]` passa a registrar a partir de 10 ms (até 1.000 linhas), com `compiled=worker(N ms)`
  quando o worker compilou; `[shader-module]` registra módulo ≥10 ms criado na Thread_Gpu.
  `resumo_rodada.py` lê os dois. Sem teste unitário: o cache de programas precisa de device Vulkan.
  Na sessão fria da fase 1 havia 52 compilações ≥50 ms na Thread_Gpu (5,4 s; a maior, um CS de 1,3 milhão
  de palavras, 403 ms).
- **Fase 4 instalada (25/10, build 00:24 em `fase1`, backup `kyty_emulator.1630.exe/.pdb`):** `Play.bat` com
  `KYTY_SHADER_WORKERS=1`, `KYTY_URGENT_FAST_COMPILE=0` e `KYTY_COMPUTE_FAST_COMPILE=0`. O exe leva também uma
  mudança no `SpirvBuilder` (chaves de tipo/constante na pilha, 24/10 16:38–16:42, de fora desta sessão; deve
  gerar o mesmo SPIR-V), que muda o `SourceVersion` para `0xfb460599a7371753`: a primeira sessão retraduz tudo.
  Medir: `[shader-compile]` deve ficar quase todo abaixo de 10 ms com `compiled=worker(...)`;
  `[pipeline-wait]` sem `unoptimized`.

## Não verificado

- quanto da compilação é do VS e quanto do PS;
- se o viewport entra na chave do VS (um agente apontou; não confirmado);
- o fps baixo fora dos travamentos: `prof_60` mostra `DrawPrepPixelState` e `DispatchDirect`, que não são
  compilação; é outro trabalho;
- a fase 4 em jogo: se a suspensão em fatias com tradução no worker não atrasa demais os dispatches de
  compute pequenos (cada shader novo passa por duas voltas do pacote).

## Referências

- GPL: [proposta](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_graphics_pipeline_library.adoc),
  [blog da Khronos](https://www.khronos.org/blog/reducing-draw-time-hitching-with-vk-ext-graphics-pipeline-library)
- Controle de cache: [pipeline creation cache control](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pipeline_creation_cache_control.html),
  [blog](https://www.khronos.org/blog/bringing-explicit-pipeline-caching-control-to-vulkan)
- [Proposta do pipeline binary](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_pipeline_binary.adoc)
- AMD: [RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/),
  [driver xgl](https://github.com/GPUOpen-Drivers/xgl/blob/dev/icd/api/vk_pipeline.cpp),
  [manual do RGA](https://gpuopen.com/manuals/rga_manual/help_manual/)
