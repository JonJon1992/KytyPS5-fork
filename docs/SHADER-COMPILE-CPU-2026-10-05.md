# Compilação de shaders: CFG e análise EXEC — 2026-10-05

Foram mantidas duas otimizações de CPU: bitsets temporários no cálculo de dominadores/post-dominadores e reutilização de memória na análise EXEC. SSA e emissão SPIR-V foram perfiladas, mas seus candidatos foram descartados por ganho inconsistente ou regressão. Nenhuma alteração de instruções, IDs ou requisitos Vulkan foi mantida.

## 1. Hotspots encontrados

O perfil por fase do corpus sintético apontou análise EXEC como maior custo de `CompileProgram`; cálculo de dominância repetido durante a estruturação contribuiu para `TranslateProgram`. No perfil inicial sem dumps, a soma dos quatro shaders foi aproximadamente 128,9 ms em Translate e 198,1 ms em Compile. Esses números variam com carga e frequência da máquina e não servem como comparação A/B final.

O corpus contém quatro compute shaders gerados pelo benchmark existente, com loops, branches mascarados por EXEC, loads e ALU. Tamanhos: 276, 1.308, 4.652 e 9.276 dwords guest. Não foi localizado um corpus de jogos disponível para medir esta mudança.

## 2. Causas

Em CFG, cada interseção percorria vetores ordenados de IDs. A estruturação recalcula dominância após transformações, multiplicando esse trabalho. O problema não era apenas a busca de um bloco por ID.

Na análise EXEC do maior shader, a instrumentação contou 142.164 alocações de memo, 19.590 remoções entre passes e 24.972 alocações do vetor de DFS. Houve 11.528 consultas de DFS, profundidade máxima 536, 290.820 lookups e dois passes de fixpoint. Os testes de implicação e classificação continuam necessários.

## 3. Alterações mantidas

`ShaderCFG.cpp`: interseção por palavras de 64 bits numa matriz contígua temporária. Preserva inicialização, maior fixpoint, ordem de visita e resultados de ciclos sem saída. A publicação continua usando vetores de IDs em ordem crescente. Não muda a representação pública do grafo. O armazenamento auxiliar de conjuntos usa aproximadamente 1/32 dos bytes de IDs `uint32_t` na inicialização anterior; os vetores publicados continuam tendo custo quadrático no pior caso.

`ExecSelectElimination.cpp`: vetor DFS reutilizado, `std::pmr::unsynchronized_pool_resource` local à análise e memo invalidado por geração. Verdicts `No` permanecem válidos; os demais são reiniciados quando consultados após mudar de passe. A geração não muda durante um passe. O limite de oito passes e o fallback conservador permanecem. RAII destrói o mapa antes do pool; não há estado global compartilhado nem novos locks.

O benchmark passou a explicitar `dump_ir=0|1`, com padrão 0 igual ao caminho de produção sem logging. A mudança da opção de dump é uma correção de medição, não um ganho atribuído às otimizações. Ele também exige igualdade literal entre rodadas e pode exportar módulos para comparação entre executáveis.

## 4–5. Antes/depois e ganho reproduzível

CFG: cinco pares A/B alternados, três rodadas por processo, CPU 0, mesmo compilador e driver, `dump_ir=1` nos dois. Medianas da soma dos quatro shaders:

| Fase | Antes | Depois | Redução |
|---|---:|---:|---:|
| BuildGraph + Structurize | 26,002 ms | 20,654 ms | 20,6% |
| Dominadores | 6,812 ms | 4,880 ms | 28,4% |
| Post-dominadores | 8,062 ms | 4,850 ms | 39,8% |
| RecomputeAnalysis | 17,333 ms | 12,665 ms | 26,9% |

As linhas se sobrepõem; não somar seus tempos. O ganho de CFG agregado foi positivo em todos os cinco pares. Esses logs não separam o tempo de CFG por shader. No menor shader, Translate completo variou de 1,256 para 1,292 ms (+2,9%, diferença de 36 µs com grande dispersão); não se comprovou ganho nesse caso. Nos três maiores, Translate caiu aproximadamente 6,3–6,5% nesse A/B de CFG isolado.

EXEC: 30 rodadas alternadas dentro do mesmo processo, duas variantes, quatro shaders; clones, comparação de IR, emissão e validação ficam fora do intervalo cronometrado. Medianas de tempo de CPU:

| Corpus | Antes | Depois | Redução |
|---|---:|---:|---:|
| 2 loops / 8 sections / 8 ALU | 0,4071 ms | 0,3615 ms | 11,2% |
| 4 / 16 / 12 | 3,8769 ms | 3,3824 ms | 12,8% |
| 8 / 24 / 16 | 25,5870 ms | 20,3518 ms | 20,5% |
| 12 / 32 / 16 | 83,2239 ms | 60,6993 ms | 27,1% |
| Soma das medianas | 113,0949 ms | 84,7950 ms | 25,0% |

Ambiente: Intel Core i7-10750H, seis cores/doze threads, WSL2 Ubuntu 24.04, Clang 18, C++20, Release `-O3 -DNDEBUG`. Driver de testes grande compilado em `-O0`; todos os objetos cronometrados do recompilador em `-O3`. Compilação a partir de cópias nativas em `/tmp`, conferidas contra o código do workspace, com `-Werror=null-character`.

A/B final combinado: fontes baseline originais e CFG+EXEC atuais, SSA/backend originais, sete pares alternados, cinco rodadas por processo, uma execução de warmup por variante. Foram testados separadamente `dump_ir=0` e `1`, com comparação literal dos quatro módulos e determinismo entre todas as rodadas. Medianas do corpus:

| Métrica | Antes | Depois | Redução observada |
|---|---:|---:|---:|
| Translate, CPU por thread, dump=0 | 160,159 ms | 144,766 ms | 9,6% |
| Compile, CPU por thread, dump=0 | 228,092 ms | 197,109 ms | 13,6% |
| Total, CPU por thread, dump=0 | 383,310 ms | 352,051 ms | 8,2% |
| Total, tempo de parede, dump=0 | 520,417 ms | 355,622 ms | 31,7% |
| Total, CPU por thread, dump=1 | 453,821 ms | 391,987 ms | 13,6% |
| Total, tempo de parede, dump=1 | 500,127 ms | 406,968 ms | 18,6% |

Cada linha é a mediana da sua série; o total é calculado por processo antes da mediana, não pela soma das medianas das duas fases. O relógio `CLOCK_THREAD_CPUTIME_ID` foi usado somente no harness temporário, preservando também os executáveis com `steady_clock`.

Houve variação elevada mesmo medindo CPU por thread; esse relógio exclui descheduling, mas não normaliza frequência, cache ou contenção de memória. No total CPU sem dumps, a variação por par foi de -9,1% a +26,5% de ganho; no tempo de parede, de -28,1% a +36,4%. Portanto os percentuais combinados são observações deste ambiente, não uma garantia de ganho nessa proporção. A decisão de manter as mudanças se apoia nos A/B isolados reproduzíveis e na equivalência literal, e não na queda maior do tempo de parede.

As metas globais de Translate <100 ms e total <250 ms, ideal <180–200 ms, **não foram comprovadas**. Os ~94 ms históricos de CFG não são o baseline atual deste corpus: ele já inclui a melhoria anterior da ordem reversa de post-dominadores, descrita em `docs/perf-research/raw-findings.md`. Não se deve afirmar 94 → 20 ms para esta alteração. O corpus atual já tinha CFG abaixo de 60 ms.

## 6. Impacto no SPIR-V e na GPU

Os quatro módulos do benchmark têm respectivamente 5.740, 23.871, 78.567 e 156.243 palavras, totalizando 1.057.684 bytes. Permaneceram literalmente iguais. A suíte ampliada também produziu 204 módulos validados e byte-idênticos entre baseline e CFG+EXEC.

Não houve redução de SPIR-V nem medição de FPS, VGPR/SGPR, occupancy ou tempo de GPU. Os ganhos são de compilação na CPU. Mudanças de instruções exigiriam medir register pressure e comportamento no hardware, conforme a documentação de [occupancy da AMD](https://gpuopen.com/learn/occupancy-explained/).

## 7. Riscos e limitações

O pool e a invalidação por geração retêm entradas até terminar a análise; isso pode aumentar memória retida entre passes. O pico de memória não foi quantificado. Não há retenção entre shaders. O caminho de DFS continua sequencial; ele não é reentrante, como o anterior.

As funções de dominância continuam dependendo dos IDs densos já garantidos pelo grafo. Bitsets reduzem o custo das interseções, mas não tornam todo o algoritmo de fixpoint linear. Não se reivindica validação de desempenho no Windows/MSVC nem em jogos.

Quatro falhas existentes aparecem igualmente em baseline e otimizado: bounds de buffer fora da guarda EXEC; formação de ponteiro de storage antes da guarda de bounds; declaração de imagens não relacionadas em sample 2D; decode de opcode MIMG atomic. A suíte normal aborta na primeira; a execução diagnóstica com `KYTY_CFG_TESTS_CONTINUE=1` permite comparar os demais casos. Esta execução não significa suíte verde. Verificações não foram removidas.

## 8. Validação e reprodução

- 2.015 grafos A/B: vetores dom/postdom, backedges, natural loops, SCCs e status literalmente iguais; 18.163.972 bytes comparados. Inclui 2.000 grafos aleatórios, grafos irreducíveis, cadeias e ciclos sem saída. SHA-256 do snapshot: `0b1aae31b724d95a82d528e41f691ad4bc84dc09ff72b25949f2d29bb9a056ab`.
- ASan/UBSan no CFG: todos os 2.015 grafos comparados, sem achados.
- EXEC: 240 execuções A/B com IR, estatísticas e SPIR-V literalmente iguais; validação SPIR-V em Vulkan 1.2 aprovada.
- EXEC sob ASan/UBSan/LSan: 24 comparações, IR/statísticas/SPIR-V iguais, `detect_leaks=1`, sem achados.
- Seletores CFG, SSA/upsync2, EXEC, mesh, wave e registered-code aprovados nos dois executáveis. ISA falha igualmente no baseline.
- Suíte completa normal: mesma falha e 107 módulos iguais até o abort. Diagnóstico completo: mesmas quatro falhas, 204 módulos validados byte-idênticos.
- Testes persistentes novos: limites de palavras 63/64/65/127/128/129; ciclo EXEC mascarado; valor congelado; observação que impede eliminação; invalidação entre passes; fallback após oito passes.

Após compilar os alvos existentes, exemplos no Linux/WSL, com o shell chamado por `rtk proxy` neste ambiente:

```text
taskset -c 0 _Build/linux/shader_recompiler_compute_tests --compile-benchmark 5 0
taskset -c 0 _Build/linux/shader_recompiler_compute_tests --compile-benchmark 5 1
_Build/linux/shader_cfg_tests --post-dominators-only
_Build/linux/shader_cfg_tests --exec-select-analysis-only
```

Definir `KYTY_SPIRV_SNAPSHOT_DIR` para diretórios separados em cada build permite comparar os arquivos `.spv` integralmente. O digest impresso sozinho não é prova de identidade. Os artefatos locais, ignorados pelo Git, estão em `_Build/shader-cfg-final-validation`, `_Build/ir-exec-within.log`, `_Build/ir-exec-sanitize.log`, `_Build/shader-final-ab/results.json`, `_Build/shader-cpu-final-ab/results.json` e scripts `_Build/cfg_profile_*.py`, `_Build/ir_same_process_build.py`, `_Build/shader_final_build.py`, `_Build/shader_final_ab.py`, `_Build/shader_cpu_bench_build.py`, `_Build/shader_cpu_final_ab.py`.

Context7 foi usado pela CLI oficial, sem instalação global ou alteração de dependências do projeto:

```powershell
rtk proxy npx.cmd --yes --cache .\_Build\ctx7-cache ctx7@latest library vulkan --json
rtk proxy npx.cmd --yes --cache .\_Build\ctx7-cache ctx7@latest docs /khronosgroup/vulkan-guide "robustBufferAccess core limits bounds checking" --json
rtk proxy npx.cmd --yes --cache .\_Build\ctx7-cache ctx7@latest docs /isocpp/cppcoreguidelines "Per.6 Per.14 Per.16 R.1 RAII CP.3" --json
rtk proxy curl --silent --show-error --fail --location --output _Build/amd-occupancy.html https://gpuopen.com/learn/occupancy-explained/
```

As recomendações de profiling, estruturas compactas, menos alocações e RAII seguem as [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines). Os checks de memória guest foram preservados: `robustBufferAccess` core tem garantias menos fortes que `robustBufferAccess2`, conforme a [documentação Vulkan](https://docs.vulkan.org/guide/latest/robustness.html). Regras de estruturação e phi foram consultadas na [especificação SPIR-V](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpPhi) e no [ambiente SPIR-V do Vulkan](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

## 9. Próximo maior gargalo

A análise EXEC ainda é o maior custo nos shaders grandes: no A/B final de CPU sem dumps, suas medianas por corpus foram 155,8 → 104,1 ms. Depois vêm emissão/validação de IR, SSA e resource tracking. No executável otimizado, o perfil final marcou emissão em aproximadamente 71,0 ms, incluindo 33,0 ms de validação de IR, SSA 26,9 ms e resource tracking 25,0 ms por corpus; custos inclusivos não devem ser somados. O perfil inicial mais leve marcou respectivamente 58,8/28,1/23,5/21,0 ms, reforçando a necessidade de controlar o ambiente na próxima medição.

Não foi acrescentada paralelização de passes que alteram o mesmo IR: eles compartilham definições/usos e dependem da ordem. Paralelizar shaders independentes é uma unidade mais segura, já existente no agendamento do projeto. Não houve evidência para adicionar outra fila ou locks.

Descartes medidos: SSA com atalhos e phi direto variou de regressão a ganho pequeno, sem benefício reproduzível; grupo de reservas/try_emplace/escrita de phi na emissão passou de 48,341 para 50,338 ms, com regressão de 7,1% no maior shader. O suposto O(n²) do DCE não se confirmou no corpus: 3.262 buscas fizeram apenas 46 comparações. Nenhum desses candidatos foi mantido.
