# Consulta de pipelines — 2026-10-05

## Evidência e escopo

Análise de `src/graphics/host_gpu/renderer/pipeline/`, na base `23753a62`.
As medições históricas abaixo orientam o alvo; não são medições deste patch.

- `DIVISAO-TRAVAMENTOS.md`, seção “Entrega e janela liberada”: no trace frio,
  707 pipelines gráficos, mediana 47,397 ms, p95 224,633 ms, 57,108 s de criação
  em 66,838 s de pausas. No trace aquecido, mediana 0,097 ms, mas ainda com misses.
  As execuções não foram pareadas por pipeline. Isso identifica compilação no
  driver como causa importante dos travamentos frios.
- `CLAUDE-DRAW-HOTPATH-2026-10-04.md`: com U59, o resolver do Astro chegou a
  aproximadamente 3,35 µs/draw; o caminho de buffers respondeu por cerca de 21%.
  O perfil do Crash também concentra custos em cópias/sincronização. Nenhum desses
  perfis atribui uma porcentagem isolada à consulta de pipelines.
- `perf-research/raw-findings.md` identifica construção/hash da chave entre os
  custos repetidos por draw. Parte das propostas antigas já existe no código atual:
  normalização, estado dinâmico, layout interning, prefetch e memos de draw-prep.

Este patch elimina trabalho redundante na consulta de objetos já criados.
Ele não reduz o tempo intrínseco do compilador Vulkan de um shader nunca visto.
O prefetch continua sujeito à antecedência disponível e ao limite de sua fila.

## Alterações

1. O hash XXH3 dos 126 bytes de `PipelineStaticParameters` passa a ser usado
   também sem `KYTY_RENDERER_BATCH`. A estrutura é packed e possui asserts de
   layout/tamanho; sua igualdade já compara esses mesmos bytes. O hash apenas
   indexa mapas em memória; não altera o formato dos arquivos de cache.
2. `KYTY_PIPELINE_MEMO` mantém o atalho da última chave e acrescenta 512 buckets
   de duas entradas, 1.024 objetos no total. Chaves recorrentes podem evitar o
   mutex e a consulta ao mapa mesmo após uma troca de pipeline. Os buckets usam
   substituição da inserção mais antiga; saturação e colisões caem no mapa normal.
3. O memo guarda ponteiros para chaves imutáveis dos nós do `unordered_map`,
   evitando copiar a chave inteira nas trocas. Rehash preserva esses endereços.
   Proprietário e geração são conferidos antes de ler qualquer chave. A geração
   deve avançar monotonamente, como já acontece no `PipelineCache`, e permanece
   exclusiva de cada instância. Substituições GPL continuam invalidando o memo;
   os objetos retirados permanecem vivos pelo contrato existente.
4. Draw-prep usa o mesmo helper, com atalho para a última chave e buckets de duas
   entradas. Um miss do prefetch constrói a chave completa uma vez, reutilizando-a
   na consulta e na reserva. A segunda checagem sob o mutex continua presente.
5. A igualdade de `PipelineVertexInputState` compara contagens e entradas ativas,
   como o hash e a criação Vulkan já fazem. Pipelines mesh com zero entradas deixam
   de percorrer os arrays inteiros em cada acerto. Stride, input rate, offset e
   binding ativos continuam distinguindo chaves. Contagens inválidas são limitadas
   ao tamanho do array na comparação; o construtor da chave continua rejeitando-as.

Pipelines pendentes não entram no memo. Escritas, barreiras, descritores, ordem
dos draws e estado dinâmico não mudam. O custo de TLS do memo serial passa de
uma chave para aproximadamente 40 KiB por thread. O memo de
draw-prep continua na mesma ordem de tamanho. `KYTY_PIPELINE_MEMO=0` conserva o
caminho sem esse memo para comparação.

## Validação

`PipelineLookupMemoTests.cpp` cobre alternância com hashes iguais, comparação da
chave completa, colisão de bucket, substituição, invalidação por geração/instância
com uma chave já liberada e rehash do mapa. A chave do teste não permite cópias.
O memo de uma entrada reproduziu sete falhas; o novo helper passou. ASan/UBSan
também passaram usando os runtimes GCC disponíveis no WSL.

`PipelineVertexInputStateTests.cpp` cobre campos ativos/inativos, todos os campos
que distinguem a entrada de vértices, arrays totalmente ativos e contagens fora do
limite. A comparação anterior reproduziu duas falhas; a nova passou com ASan/UBSan.

`ShaderAsyncPipelineTests.inc` acrescenta duas topologias reais, verifica retorno
dos mesmos objetos e seis hits em consultas alternadas, além das verificações
existentes de publicação, pixels e escritas vertex/fragment. A variante CTest
`async_pipeline_memo_off` exige zero hits e os mesmos objetos com o memo desligado.

Build de validação isolado: `_Build/codex-pipeline-tests`, Clang 18.1.3, Release,
WSL Ubuntu 24.04. Os backends X11/Wayland estão desligados porque suas bibliotecas
de desenvolvimento não estão instaladas; SDL usa `SDL_UNIX_CONSOLE_BUILD=ON`.
As flags Release desse build incluem `-fexceptions`: o `assert.cpp` da base usa
`try/catch`, enquanto as flags Linux globais o desabilitam. Essa correção fica
somente na configuração de teste, sem editar os defaults de produção.
Após baixar as dependências, `FETCHCONTENT_FULLY_DISCONNECTED=ON` evita reaplicar
o patch existente de ZArchive na reconfiguração. Não é uma configuração de jogo.

Para medir apenas a consulta real, o teste aceita `KYTY_TEST_PIPELINE_LOOKUP_BENCH=1`
e imprime ns/consulta em 200.000 consultas consecutivas e alternadas, depois de
aquecer os objetos. Comparar com `KYTY_PIPELINE_MEMO=0` desliga o memo inteiro;
comparar com o `pipelineCache.cpp` da base distingue o memo antigo do novo.
Esse microbenchmark não mede FPS, desenho, upload, nem compilação fria.

O WSL oferece somente `llvmpipe (LLVM 20.1.2, 256 bits)`, sem fragment barycentrics.
Essa feature é obrigatória no harness; portanto os testes de pixels não foram
executados neste ambiente. O build completo também encontrou um crash do Clang
durante alterações simultâneas em arquivos de draw-prep. Essas alterações foram
preservadas. A validação foi focada na unidade de produção `pipelineCache.cpp`,
na sintaxe de `ShaderRecompilerComputeTests.cpp` e nos testes CPU pertinentes.

`tools/benchmark_pipeline_lookup.cpp` é um microbenchmark CPU separado, com o
layout real dos campos da chave e a igualdade antiga/nova. Reproduz os mecanismos
de último objeto, hash e consulta ao mapa; usa `std::mutex` para o miss. Não chama
`TryGetGraphicsPipeline`, não cria a chave a cada draw e não mede rendering. Os
modos `old-byte` e `old-batch` representam respectivamente o hash anterior sem e
com renderer batching; `new` usa o novo memo e a igualdade dos campos ativos.
São workloads controlados de 1, 2 e 64 chaves recorrentes, com 0, 2 e 32 entradas
de vértices, três repetições e ordem de medição alternada. Resultados devem ser
interpretados apenas nesse escopo.

Resultados finais:

- Build focado de `pipelineCache.cpp`, das duas suítes novas, do benchmark e dos
  alvos `binding_path_tests` / `pipeline_compile_queue_tests`: exit 0.
- Sintaxe do harness completo, incluindo `ShaderAsyncPipelineTests.inc`: exit 0.
  Há warnings anteriores de funções de teste não utilizadas e de caminhos EXIT;
  não foi alegado um build sem warnings nem um build/link completo do emulador.
- CTest: `pipeline_lookup_memo`, `pipeline_vertex_input_state`,
  `pipeline_compile_queue`, `binding_path`: **4/4 passaram**.
- ASan/UBSan: ambas as suítes novas passaram, zero erros reportados.
- `git diff --check` dos arquivos deste patch: passou.

Microbenchmark, chave de 728 bytes, 1.000.000 consultas por medição. Medianas de
três repetições; comparação conservada contra o caminho antigo com hash rápido
(`old-batch`), que corresponde ao mecanismo usado com renderer batching:

| Entradas de vértices | Chaves no ciclo | Antes, ns/consulta | Depois, ns/consulta | Redução |
|---:|---:|---:|---:|---:|
| 0 | 1 | 77,963 | 20,301 | 74,0% |
| 0 | 2 | 125,884 | 39,916 | 68,3% |
| 0 | 64 | 155,716 | 67,412 | 56,7% |
| 2 | 1 | 82,116 | 18,186 | 77,9% |
| 2 | 2 | 134,155 | 63,153 | 52,9% |
| 2 | 64 | 168,853 | 58,976 | 65,1% |
| 32 | 1 | 65,372 | 47,258 | 27,7% |
| 32 | 2 | 295,050 | 232,198 | 21,3% |
| 32 | 64 | 270,600 | 231,729 | 14,4% |

Todas as consultas produziram os checksums esperados. A redução ficou entre
14,4% e 77,9% **no mecanismo isolado**; esses percentuais não são ganho de FPS,
nem redução equivalente do tempo de um draw ou de um frame. Ainda faltam A/B
de gameplay com o preset completo e validação dos pixels em hardware compatível.
As pausas frias do compilador permanecem uma frente distinta.

Artefatos locais: `_Build/pipeline-lookup-benchmark.{txt,json}`,
`_Build/pipeline-focused-build.log`, `_Build/pipeline-test-fixture-syntax.log`,
`_Build/pipeline-cpu-ctest.log`. Reprodução do benchmark:

```sh
cmake --build _Build/codex-pipeline-tests --target pipeline_lookup_benchmark
_Build/codex-pipeline-tests/pipeline_lookup_benchmark
```

## Referências consultadas

Context7, API HTTP pública, biblioteca resolvida como `/khronosgroup/vulkan-docs`:
consultas sobre cache/estado dinâmico e sincronização de `vkCreateGraphicsPipelines`.
Os trechos foram conferidos na documentação oficial, incluindo a exigência de
sincronização externa quando o cache é criado com essa flag.

- [Khronos: gerenciamento e reutilização de pipelines](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html).
- [Khronos: estado dinâmico e seus limites de performance](https://docs.vulkan.org/guide/latest/dynamic_state.html).
- [Especificação: pipelines e sincronização do cache](https://docs.vulkan.org/spec/latest/chapters/pipelines.html).

A recomendação de reutilizar objetos existentes sustenta o alvo deste patch.
Ampliar estado dinâmico ou ligar GPL/compilação sem otimizações requer outro A/B:
essas opções podem afetar a performance na GPU e têm restrições de correção já
documentadas no fork. Os defaults desses experimentos não foram alterados.
