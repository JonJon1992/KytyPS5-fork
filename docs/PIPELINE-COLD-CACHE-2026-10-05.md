# Cache frio da pipeline — 2026-10-05

## Evidência e alvo

As medições históricas de `DIVISAO-TRAVAMENTOS.md` mostram criação de pipelines
gráficas no driver AMD na ordem de 150–250 ms por pipeline. A descrição antiga
de ausência de async/prefetch está desatualizada: ambos existem no código atual.

A evidência mais recente de `EXPERIMENTAL.md`, seção “New in this update”, já
mede prefetch preservando todos os draws: na rota Sky Garden, o CP passou de
87–89 s de stalls para 68–70 s; na Creamy Canyon, de 76 s para 57–59 s. São
resultados anteriores a este patch, não ganhos atribuídos às alterações abaixo.
Ainda há pouco tempo de antecedência para esconder traduções e compilações novas.

A análise encontrou dois desperdícios corrigíveis sem desativar otimizações de
shader, sem depender de GPL e sem descartar draws:

1. Previsões prontas nunca consumidas ocupavam os 128 slots até o shutdown. Depois
   de suficiente divergência entre previsão e consumo, novos prefetches eram
   recusados mesmo com workers ociosos. Esse limite está descrito na versão
   anterior de `PIPELINE-STUTTER.md` e reproduzido pelo teste CPU.
2. A normalização já removia fatores de blend desligado e parâmetros de alpha
   separado inativo, mas ainda distinguia fatores de operações MIN/MAX. A equação
   Vulkan de MIN/MAX não utiliza esses fatores; tais variantes podem compartilhar
   a mesma pipeline para shaders, targets e demais estados iguais.

## Alterações

- Na capacidade máxima, o prefetch libera a previsão **concluída e não consumida**
  mais antiga e admite trabalho novo. Não espera por compilações ativas, não
  cancela tasks e mantém o limite de 128 pedidos armazenados.
- O CP reserva o resultado sob o lock do mapa antes de soltá-lo para esperar.
  Assim, outro preparador não pode liberar sua pipeline entre a reserva e o
  movimento do future. Entradas reservadas ou com future já movido são protegidas.
- Somente objetos nunca publicados nem usados em command buffers são liberados:
  `destroyPipeline` e liberação das referências de layout ocorrem após o future
  indicar conclusão. Pipelines publicadas e o memo existente não são alterados.
- Novos contadores `retired` e `saturated` aparecem em `GetPrefetchTotals()` e no
  log final do prefetch. `saturated` conta recusas por capacidade; pedidos
  duplicados não entram nesse contador. `retired` conta previsões prontas liberadas.
- Fatores RGB de MIN/MAX e fatores de alpha MIN/MAX com alpha separado são
  normalizados para zero. ADD/SUBTRACT/REVERSE_SUBTRACT e os fatores do outro
  canal permanecem intactos. O código usa `Prospero::BlendOp`, cujos valores
  numéricos diferem de `VkBlendOp`.

O ganho esperado é manter a antecipação funcionando após muitas previsões não
consumidas e evitar compilar variantes equivalentes de MIN/MAX. Não torna uma
compilação inédita do driver intrinsecamente mais rápida. O ganho depende da
incidência de saturação e dessas variantes no jogo.

A liberação tem uma contrapartida: se uma previsão descartada for necessária
mais tarde, seu objeto precisará ser recriado. O cache do driver pode amortizar
esse trabalho, mas o Vulkan não garante um cache hit. Um fluxo com previsões
muito imprecisas também pode executar mais trabalho especulativo; o A/B precisa
comparar stalls, frames e uso de CPU, não apenas o número de submissions.

## Validação

`PipelineColdCacheTests.cpp` falhou antes da implementação nos casos de recuperação
de capacidade e equivalência MIN/MAX. Com a implementação, cobre:

- capacidade de 128, escolha da previsão mais antiga pronta e admissão de novo
  pedido sem crescimento da tabela;
- proteção de um trabalho em andamento, de um resultado reservado antes de mover
  o future e de um future já entregue ao consumidor;
- preservação do resultado exato do consumidor;
- MIN e MAX distintos, independência entre RGB/alpha e preservação de fatores ADD;
- idempotência e 256 permutações equivalentes de fatores reduzidas a uma chave.

As 256 → 1 chaves são um teste sintético de equivalência, não uma medição de
pipelines eliminadas no jogo, de tempo de driver ou de FPS.

O build isolado WSL/Clang 18 compila a unidade `pipelineCache.cpp` e verifica a
sintaxe do harness Vulkan. Os cinco testes CPU são `pipeline_cold_cache`,
`pipeline_lookup_memo`, `pipeline_vertex_input_state`, `pipeline_compile_queue`
e `binding_path`: **5/5 passaram**. O teste novo também passou com ASan/UBSan,
sem erros reportados. A sintaxe do harness passou; há warnings anteriores de
caminhos EXIT e de funções de teste não utilizadas.

Não há resultado A/B de gameplay deste patch. O Vulkan disponível no WSL é
llvmpipe e não oferece fragment shader barycentrics, obrigatórios para o harness
do renderer. A validação GPU/pixels e a medição das rotas frias continuam pendentes;
um build/link completo do emulador também não foi validado nesta etapa.

## Reprodução e A/B

```sh
cmake --build <build> --target pipeline_cold_cache_tests pipeline_compile_queue_tests
ctest --test-dir <build> --output-on-failure -R '^(pipeline_cold_cache|pipeline_compile_queue)$'
```

Para isolar as alterações, manter o mesmo preset, rota, driver e configuração de
prefetch em processos separados. As duas flags são lidas no processo e começam
ligadas; variar uma por vez:

| Opção | Antes | Depois |
|---|---:|---:|
| `KYTY_PIPELINE_PREFETCH_RECLAIM` | 0 | 1 |
| `KYTY_PIPELINE_BLEND_MINMAX_NORMALIZE` | 0 | 1 |

O segundo A/B exige `KYTY_PIPELINE_KEY_NORMALIZE=1`. O primeiro exige prefetch
ativo. Seguir o procedimento de cache frio de `PIPELINE-STUTTER.md`, com cache
de aplicação independente e salt diagnóstico inédito por run; conferir a
frieza pelo tempo real de compilação. Não apagar os caches normais de gameplay.
Registrar `compile_stall_us`, `compile_stall_max_us`, tempo/número de compilações,
percentis e pior frame de `frames.csv`, junto dos contadores de prefetch.
Repetir as rotas: rotas diferentes ou um run já aquecido não formam um A/B válido.

Artefatos locais: `_Build/pipeline-cold-{build,fixture-syntax,ctest,sanitizer-build,sanitizer-run}.log`.

## Fontes

Context7: biblioteca `/khronosgroup/vulkan-docs`, consulta sobre criação paralela,
cache e compilação antecipada; as regras foram conferidas nas fontes oficiais:

- [Vulkan: equações de blend MIN/MAX](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html#framebuffer-blendoperations).
- [Vulkan: pipelines e sincronização do cache](https://docs.vulkan.org/spec/latest/chapters/pipelines.html).
- [Khronos: gerenciamento e reutilização de pipelines](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html).

O cache compartilhado continua internamente sincronizado; este patch não adiciona
`VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` nem substitui a política GPL.
