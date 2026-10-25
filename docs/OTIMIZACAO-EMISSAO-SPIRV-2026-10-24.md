# Auditoria incremental de CPU e emissão SPIR-V — 2026-10-24

Foi aplicada uma otimização no cache de declarações do `SpirvBuilder`: acertos de tipos e constantes com operandos escalares passam a usar chaves na stack. O mapa só passa a possuir um vetor quando insere uma declaração nova. O algoritmo continua usando a mesma ordenação dos words, os mesmos IDs e a mesma serialização SPIR-V.

O ganho medido foi de aproximadamente 44% no lookup isolado e 7,3% no corpo do emissor sobre as fixtures de regressão. Esses resultados não são percentuais de ganho de FPS, de tradução inteira nem de compilação do driver. Frametime P95/P99 e custo por draw em gameplay ainda precisam de uma partida normal com `frames.csv` e um perfil da mesma build.

## Evidência e prioridade

Os dados de partidas normais já registrados em [AUDITORIA-SHADERS-PIPELINES.md](AUDITORIA-SHADERS-PIPELINES.md) e [PLANO-GARGALOS.md](PLANO-GARGALOS.md) colocam a compilação do driver e suas esperas antes dos microcustos de CPU: 133 esperas registradas, p95 de 2.034 ms, e um perfil com 42% da thread GPU em `PipelineCompiler::Wait`. A repetição de um VS em 430 de 524 misses sugere uma oportunidade de reuso; um miss monolítico, por si só, não demonstra que o driver recompilou integralmente esse VS.

O primeiro experimento desta rodada atacou o SPIR-V do VS recorrente `4efbcf79c36d8adf`, já associado a muitas esperas na auditoria. Ele não justificou integrar o conjunto completo de passes `-O`. A mudança de CPU escolhida depois disso reduz trabalho de emissão sem alterar o programa GPU.

| Área | O que foi confirmado | Decisão nesta rodada |
|---|---|---|
| Pipeline cache / driver | Esperas altas já medidas; o mesmo hash guest pode produzir SPIR-V legitimamente diferente por especialização de recursos | Preservar os tipos e o estado correto; nenhum contorno que pule trabalho necessário |
| SPIR-V `-O` | Menos words, mas overhead do otimizador maior que a economia observada no driver | Não integrado |
| Emissor C++ | Uma alocação temporária em cada acerto de constante; reproduzida e contada diretamente | Otimizado no `SpirvBuilder` |
| `SrtWalker` | Já há memoização por geração, JIT, replay de fluxo/expressões e eliminação de leituras repetidas por bloco | Sem reescrita ou unrolling; falta uma nova medição que isole custo remanescente |
| `Thread_Gpu` | Tradução/emissão e materialização têm dependências de memória guest; esperas podem bloquear o atendimento de readbacks | Mantida a semântica de suspensão/retomada existente; mover trabalho exige snapshots com lifetime correto e nova medição |
| Loops de diagnóstico | `AddPermutation` conta novamente as permutações de todos os programas para imprimir totais | Candidato secundário; não há medição que justifique alterar agora |

O perfil offline fresco da suíte também contém validação e disassembly: cerca de 47% das amostras inclusivas passam pelo validador. Ele não pode ser usado como estimativa do custo dessas rotinas no gameplay. O benchmark do emissor mede seu corpo separadamente desse trabalho.

## Alteração aplicada

Arquivos: [SpirvBuilder.h](../src/graphics/shader/recompiler/backend/spirv/SpirvBuilder.h) e [SpirvBuilder.cpp](../src/graphics/shader/recompiler/backend/spirv/SpirvBuilder.cpp).

Antes, `Type` e `Constant` montavam um `std::vector<uint32_t>` antes de consultar `m_declaration_ids`. Mesmo uma constante já existente fazia allocate/free da chave. `ValueEmitContext::Def` usa esse caminho ao resolver operandos imediatos, e os helpers do emissor também o usam.

Agora, o caso de aridade conhecida usa `std::array`; um comparador transparente permite consultar o mesmo `std::map` com `std::span<const uint32_t>`. Em um miss, o mapa recebe uma chave própria. O caminho de operandos variáveis transfere seu vetor com `try_emplace`, sem fazer uma segunda cópia. `AllocateId` só avança quando a inserção é nova.

Não foram adicionados cache global, dependência, pass de otimização GPU ou estado compartilhado entre threads. O builder continua pertencendo à compilação que o criou. Os loops que avaliam recursos, CFG e sincronização permanecem necessários para a correção.

## Medições

Máquina: Ryzen 7 5700X, Windows x64, clang-cl 22.1.3. Lookup isolado: `/O2 /MD /EHsc /std:c++20`, sem LTO. Emissor completo: configuração Release do projeto com ThinLTO. Processos de benchmark presos ao mesmo processador lógico 2; ordem ABBA para reduzir viés de temperatura/frequência. Os dois binários receberam os mesmos testes e entradas.

### Cache de declarações

O benchmark preenche 1.024 constantes e faz nove rodadas de um milhão de lookups cada. A primeira rodada de cada processo aquece o processo e é descartada. Três ciclos ABBA produzem 48 amostras por variante. O checksum dos IDs precisa ser igual em todas as amostras; o contador observa as chamadas reais de `operator new` apenas no trecho medido.

Os dados estão em [performance/2026-10-24-spirv-declarations-abba.csv](performance/2026-10-24-spirv-declarations-abba.csv). A comparação inicial controlada passou de 105,6 para 59,4 ns/lookup, com 1.000.000 → 0 alocações por milhão de acertos. O teste também exige que misses com operandos em span não façam mais alocações que o mesmo trabalho com escalares.

### Corpo do emissor

Uma cópia temporária da suíte executou suas fixtures cem vezes por processo. O timer começa na entrada de `Spirv::EmitProgram`, termina depois de `Builder::Build` e não inclui validação/disassembly nem a destruição posterior do estado. O hash é calculado fora desse intervalo. Foram comparadas duas execuções de cada variante em ABBA.

| Variante | Módulos por execução | Words por execução | Tempo de emissão |
|---|---:|---:|---:|
| Antes, execução 1 | 25.000 | 12.089.200 | 439,6513 ms |
| Depois, execução 1 | 25.000 | 12.089.200 | 408,0007 ms |
| Depois, execução 2 | 25.000 | 12.089.200 | 404,2069 ms |
| Antes, execução 2 | 25.000 | 12.089.200 | 436,9082 ms |

Média: **438,2798 → 406,1038 ms**, redução de **7,34%** no corpo medido. O hash acumulado dos words foi `39ed93f21b959d55` nas quatro execuções. Há também um teste com words esperados literalmente para a serialização das declarações. Dados: [performance/2026-10-24-spirv-emission-abba.csv](performance/2026-10-24-spirv-emission-abba.csv).

As fixtures são pequenas e cobrem formas de instrução, não a distribuição real de tamanho dos shaders do jogo. Não extrapolar os 7,34% para todo shader. O número de instruções/palavras permaneceu igual, como esperado de uma mudança no armazenamento de chaves.

### Experimento descartado: passes completos `-O`

No VS capturado de 72.867 words, `RegisterPerformancePasses()` produziu 62.201 words, redução de 14,6%, e passou pelo validador Vulkan 1.3 antes e depois. Porém, o otimizador levou 410,665 ms. O probe do driver mediu medianas de 940,44 ms para o original e 881,25 ms para o transformado: cerca de 59 ms de economia observada para mais de 410 ms de trabalho adicional.

Esse probe usa `VK_EXT_shader_object`, que tem um caminho diferente do PSO monolítico usado no emulador. Ele também adiciona `OpModuleProcessed`, um registro de debug que o driver pode ignorar ao montar sua cache; portanto, o probe não garante cache frio. A descrição de [PipelineBuildTimer.cpp](../tests/PipelineBuildTimer.cpp) foi corrigida para explicitar isso. Esses números são exploratórios e não justificam habilitar o otimizador no gameplay.

Dois subconjuntos também foram medidos offline e validados, sem integração: simplificação de instruções produziu 70.595 words em 55,981 ms; redundância local + DCE produziu 67.730 words em 64,622 ms. Falta medir seu custo líquido em PSOs reais. Dados: [performance/2026-10-24-spirv-opt-probe.csv](performance/2026-10-24-spirv-opt-probe.csv).

## Testes e reprodução

[SpirvBuilderTests.cpp](../tests/SpirvBuilderTests.cpp) verifica reutilização de IDs, literais com sinal, equivalência entre operandos escalares e spans, operandos vazios, diferenças de comprimento, tipos decorados, serialização e alocações dos caminhos aquecidos. Antes da mudança, o teste de acertos falhou com **8.192 alocações** para 4.096 pares de lookup. O modo `--benchmark` roda independentemente desse teste, permitindo medir o código anterior com o mesmo harness.

```powershell
cmake --build _Build/port-lto --target spirv_builder_tests shader_cfg_tests -- -j8
_Build/port-lto/spirv_builder_tests.exe
_Build/port-lto/spirv_builder_tests.exe --benchmark
_Build/port-lto/shader_cfg_tests.exe
```

Para comparar dois executáveis Release construídos com o mesmo harness e flags:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/benchmark_spirv_builder.ps1 `
  -BaselineExe <binario-anterior> -CandidateExe <binario-novo> `
  -OutCsv _Build/perf-audit/comparacao.csv -Cpu 2
```

As cópias de fontes anteriores, executáveis A/B, fontes instrumentadas e logs desta rodada estão em `_Build/perf-audit`. A instrumentação temporária de emissão não foi incorporada ao código do emulador.

## Compatibilidade e limitações

O mecanismo existente de `shader_cache_version.cmake` calcula a versão a partir das fontes do recompiler. Esta alteração muda essa versão mesmo quando as fixtures emitem os mesmos bytes. Na próxima build, entradas do cache de tradução da versão anterior deixam de ser usadas. O cache opaco do driver continua seguindo sua própria assinatura de dispositivo/driver e pode reutilizar pipelines cujos bytes e estados não mudaram. O custo da primeira sessão após essa mudança não deve ser comparado diretamente com uma sessão quente anterior.

A validação de fixtures e a identidade de saída reduzem o risco gráfico deste incremento. Ainda não há medição antes/depois de P95/P99 em uma partida normal com esta build. Esse é o limite da evidência atual; o ganho comprovado é no emissor e no lookup.

## Próximos candidatos e correção da auditoria

O próximo experimento de maior impacto é GPL com os mesmos layouts completos compatíveis. A especificação permite esse caminho sem `INDEPENDENT_SETS`; um descriptor set por estágio não é obrigatório. A auditoria anterior foi corrigida nesse ponto. É necessário medir o reuso real das bibliotecas e o custo de link antes de mudar o modelo de descriptors.

Canonicalizar imagens 2D/array ou classes numéricas pode eliminar permutações, mas exige preservar coordenadas, seleção de camada, views e tipos de resultado. Diferença de SPIR-V para o mesmo hash guest pode ser uma especialização válida. Não tratar todo miss como instabilidade nem fundir variantes sem essa prova.

Para `SrtWalker` e CPU por draw, a próxima medição deve separar replay/JIT, materialização, leitura guest e espera por GPU. Só o trabalho que continuar relevante nesses contadores justifica outra otimização. O CSV por present existente deve fornecer P95/P99; o `fps.csv` de uma amostra por segundo não permite recuperá-los.

## Fontes oficiais consultadas

- [Khronos — ambiente SPIR-V do Vulkan](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/spirvenv.adoc): validação e controles de ponto flutuante.
- [SPIRV-Tools — contrato e custo dos passes](../3rdparty/SPIRV-Tools/include/spirv-tools/optimizer.hpp) e [validador](../3rdparty/SPIRV-Tools/README.md): fonte da versão instalada, usada no experimento.
- [Khronos — GPL, layouts e linking](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_graphics_pipeline_library.adoc): opção de fornecer o layout completo sem independent sets.
- [AMD GPUOpen — RDNA Performance Guide](https://gpuopen.com/learn/rdna-performance-guide/): criação e reuso de pipelines.
- [AMD GPUOpen — port e custo de pré-compilar pipelines](https://gpuopen.com/learn/porting-detroit-1/): SPIR-V pronto não elimina o trabalho do driver.
