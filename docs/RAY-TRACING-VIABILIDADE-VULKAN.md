# Análise do Ray Tracing do emulador PS5 e viabilidade de aceleração Vulkan

Data da análise: 5 de outubro de 2026.

Referência do código: workspace baseado em `396fe310`, incluindo as alterações locais preexistentes durante a análise. Este documento registra o relatório produzido naquela inspeção; alterações posteriores do workspace exigem revalidação das conclusões sobre o código atual.

**É tecnicamente viável acelerar parte do RT usando Vulkan, mas não substituir genericamente cada instrução BVH do PS5 por uma Ray Query mantendo equivalência garantida.** A arquitetura mais promissora combina emulação fiel por instrução com substituição seletiva de regiões completas de traversal.

Há uma constatação essencial: **na árvore examinada, os dispatches e draws cujos shaders contêm essas instruções são ignorados. O RT correspondente não está sendo executado em software nem em hardware.** Por isso, qualquer medição futura deve comparar RT software funcional contra RT acelerado equivalente, nunca contra RT descartado (§13).

> **Revisão de 5 de outubro de 2026 (HEAD `a7197762`).** As afirmações de código foram reconferidas e se mantêm. Correções: (1) até esta revisão, um shader **não compute** com BVH abortava o emulador no construtor de CFG. Agora ele é descartado como no compute (§1). (2) O cache de vereditos já é invalidado automaticamente pelo fingerprint do recompilador (§2).

A investigação foi somente de leitura: não houve alterações de código, compilação, execução de jogos ou criação de commits. Este arquivo foi salvo posteriormente a pedido do usuário. Foram consultados o código local, a documentação Vulkan pelo Context7 e referências oficiais AMD, Khronos e NVIDIA, seguindo as skills CppStudio e Context7.

As classificações usadas são:

- **Código:** comportamento confirmado na implementação examinada.
- **Documentação:** contrato externo ou relato histórico; não comprova execução nesta árvore.
- **Inferência:** conclusão técnica sustentada pelas evidências.
- **A validar:** requer testes, captura ou engenharia reversa.

Os links locais apontam para arquivos do repositório. Os nomes de funções identificam os pontos examinados, pois os números de linha podem mudar.

## 1 Como o RT funciona atualmente

**Código:** `DecodeMimg` identifica a família, o opcode bruto, o tamanho da instrução e os operandos NSA. Porém, os opcodes RT não possuem tradução funcional nessa tabela: permanecem como instruções não implementadas.

Em seguida, `DecodeProgram` reconhece especificamente:

| Instrução | Opcode MIMG | Comportamento atual |
| --- | --- | --- |
| `IMAGE_BVH_INTERSECT_RAY` | `0xE6` | Marca `has_bvh` e interrompe a decodificação |
| `IMAGE_BVH64_INTERSECT_RAY` | `0xE7` | Mesmo comportamento |

`TranslateProgram` verifica `decoded.has_bvh` **antes da construção do CFG** e retorna `skip_dispatch = true`, em qualquer estágio.

O pipeline cache transforma esse resultado em ausência de programa executável; os caminhos de dispatch direto e indireto retornam sem executar o shader.

Referências: [`DecodeMimg`](../src/graphics/shader/recompiler/frontend/decode/ImageOps.cpp), [`DecodeProgram`](../src/graphics/shader/recompiler/frontend/decode/ShaderDecoder.cpp), [bloqueio em `TranslateProgram`](../src/graphics/shader/recompiler/ShaderRecompiler.cpp).

Isso significa que:

- Não há interseção CPU substituindo essas instruções.
- Não há traversal compute funcional associado a elas nesta árvore.
- Não há emissão de SPIR-V Ray Query para elas.
- O dispatch inteiro é omitido, inclusive eventuais escritas não relacionadas diretamente à interseção.

**Código (corrigido nesta revisão):** antes, o descarte valia só para compute. Como `DecodeProgram` trunca o programa na instrução BVH em qualquer estágio, um VS/PS com BVH chegava a `CFG::BuildGraph`, encontrava `Opcode::UNSUPPORTED` e chamava `ExitBuildFailure` → `EXIT` + `std::abort()`: **o emulador encerrava**. Agora o descarte vale para todos os estágios. Nos gráficos, o draw é descartado pelo mesmo caminho já usado para descritores não resolvidos (`renderDraw.cpp`, "draw dropped"). Limitação conhecida, que também já existia para descritores não resolvidos: o descarte do draw só verifica `vertex[0]` e `pixel`. Um hull/domain shader com BVH em `vertex[1]`/`vertex[2]` não é tratado. Não existe backend RT alternativo em nenhum estágio.

## 2 Fluxo completo de execução

O caminho atual é:

```text
Comandos PM4 do PS5
  → CommandProcessor
  → RenderExecutor::DispatchDirect / DispatchIndirect
  → PipelineCache::GetComputeProgram
  → Decoder RDNA
  → encontra MIMG 0xE6 / 0xE7
  → has_bvh = true
  → TranslateProgram retorna skip_dispatch
  → pipeline cache retorna programa nulo
  → dispatch não é executado
```

O veredito de descarte também é persistido no cache de programas em disco. Isso **já é coberto**: a chave do cache inclui `CodegenFingerprint`, um hash gerado no build a partir das fontes do recompilador (`programDiskCache.h`, `CMakeLists.txt`). Uma implementação futura invalida os vereditos antigos automaticamente, desde que fique em arquivos incluídos nesse hash.

Referências: [`CommandProcessor::ExecDispatchIndirect`](../src/graphics/guest_gpu/graphicsRun.cpp), [cache do descarte](../src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp), [`RenderExecutor::DispatchDirect` e `DispatchIndirect`](../src/graphics/host_gpu/renderer/renderCompute.cpp).

Para shaders que passam pela tradução, a infraestrutura geral é:

```text
ISA PS5
  → decoder
  → CFG e structurizer
  → IR
  → SSA e passes de simplificação
  → planejamento/materialização dos recursos
  → especialização e bindings
  → emissão SPIR-V
  → otimização SPIR-V
  → compilação do pipeline pelo driver Vulkan
  → execução GPU
```

**Documentação:** no modelo AMD relevante, a instrução BVH realiza um **teste de nó**. Pilha, escolha do próximo nó, transformações de instância e atualização do resultado são conduzidas pelo shader ao redor dela. Isso é fundamental para entender por que a substituição por Ray Query não é direta. A referência pública GPURT distingue testes de boxes, triângulos e nós definidos pelo software. [GPURT IntersectCommon](https://github.com/GPUOpen-Drivers/gpurt/blob/dev/src/shaders/IntersectCommon.hlsl).

## 3 O que já funciona

**Código:**

- Detecção dos dois opcodes RT.
- Reconhecimento do comprimento das instruções, inclusive NSA.
- Interrupção antes de tentar traduzir o shader compute incompatível.
- Cache do resultado de descarte.
- Infraestrutura geral de IR, SPIR-V, compute, BDA e coerência de buffers.

Existe um teste, `TestRayTracingDispatchDetection` (incluído por `shaderCfgTests.cpp`), que verifica a detecção, incluindo falsos positivos em literais, palavras de operandos e dados posteriores ao fim do shader. Ele também verifica que um shader BVH não chega ao IR em compute, vertex e pixel. Cobre apenas detecção e descarte, sem semântica de interseção. [Testes atuais](../tests/ShaderRayTracingTests.inc).

É necessário separar isso dos trabalhos de outra branch registrados no repositório:

| Evidência histórica | O que relata | Situação nesta análise |
| --- | --- | --- |
| `KYTY_RT_SOFTWARE` e `BvhIntersectRay` | Emulação da instrução e emissão SPIR-V | Ausentes do código ativo |
| `psrBvh` e `rtAccel` | Conversão de estruturas e construção Vulkan | Ausentes do código ativo |
| `rt_hardware_tests` | Comparação software/híbrido/hardware | Relato histórico, sem reprodução |
| Integração de cenas RT | Preparação de snapshots e inicialização | Registrada como WIP |

O próprio catálogo declara que a integração histórica não estabeleceu aceleração de Astro Bot com hardware RT. Consulte as entradas “Hardware RT prototype: Psr BVH conversion and ray-query kernels (tests only)” e “WIP: snapshot native RT scene preparation and device startup integration” no [catálogo de alterações](CHANGE-CATALOG.md).

## 4 O que está incompleto

Na árvore examinada:

| Componente | Estado confirmado |
| --- | --- |
| Decodificação semântica completa BVH/BVH64 | Não implementada |
| Intrinsic RT no IR | Ausente |
| Interseção box16/box32 | Ausente como operação RT |
| Interseção de triângulos | Ausente como operação RT |
| Instâncias RT | Sem implementação integrada |
| Traversal guest contendo essas instruções | Compute descartado |
| Backend software RT | Ausente |
| Backend Vulkan Ray Query | Ausente |
| Espelho BLAS/TLAS e sua invalidação | Ausente |

A infraestrutura de recursos dinâmicos já tem mecanismos úteis: por exemplo, determinados descritores calculados durante o shader podem ser encaminhados para BDA. Isso não equivale a suporte completo a todos os builders e traversals de RT. [ResourceTracking](../src/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp).

**Inferência:** implementar apenas a instrução de interseção seria insuficiente. Também seria necessário validar os shaders que constroem, compactam e atualizam o BVH. Uma interseção correta sobre uma estrutura incompleta continua produzindo resultados incorretos.

## 5 Principais gargalos

**No estado examinado, o bloqueio principal é funcional.** Não há custo de execução dos dispatches RT descartados a otimizar.

Para um caminho software futuro, os principais candidatos são:

| Área | Evidência e consequência |
| --- | --- |
| Compilação CPU | CFG complexo, SSA volumoso e backend do driver podem causar compilações longas |
| Execução GPU | Traversal divergente, pilhas, matemática de interseção e baixa ocupação |
| Memória GPU | Leituras indiretas de nós, tradução de endereços e baixa localidade |
| Coerência | CPU lendo estruturas produzidas pela GPU pode exigir publicação/readback |
| Sincronização | Esperas podem colocar construção, conversão e traversal em série |
| Hardware RT futuro | Extração de geometria, build/update de BLAS/TLAS e manutenção de versões |

**Código:** o caminho genérico `LoadBda` pode incluir predicação, resolução pela tabela de páginas, teste de presença e tratamento de acesso desalinhado. Repetir esse caminho para cada dword de um nó seria caro. A tabela usa páginas de 16 KiB. [Leituras BDA](../src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp), [tamanho das páginas](../src/graphics/host_gpu/renderer/cache/bufferCache.h).

**Inferência:** um backend software deveria tentar resolver o endereço do nó uma vez e agrupar leituras, respeitando alinhamento, páginas e validade. Isso reduziria overhead sem substituir o traversal guest.

**Código:** já existem side readbacks e readbacks antecipados. Portanto, não é correto transportar para esta versão a afirmação histórica de que todo readback exige sempre um drain completo. Ainda existem caminhos que aguardam publicação ou leem dados GPU-dirty. [BufferCache](../src/graphics/host_gpu/renderer/cache/bufferCache.h), [sincronização do backing](../src/graphics/host_gpu/renderer/renderContext.cpp).

O dispatch indireto normal usa argumentos na GPU. Existe uma exceção para `USE_THREAD_DIMENSIONS`, que lê argumentos pelo command processor.

**A validar:** qual desses mecanismos domina o tempo de frame quando RT realmente estiver executando. Utilização GPU baixa e CPU em espera, isoladamente, não identificam a causa.

## 6 Problemas de SPIR-V e CFG relacionados ao RT

Há três problemas distintos.

**Estruturação de saídas de loops**

**Código:** `IsInnermostLoopControlConditional` ainda considera como controle do loop uma condicional com um destino dentro do corpo e outro fora. Posteriormente, essa classificação dispensa a seleção de um merge para a condicional. [Classificação e structurizer](../src/graphics/shader/recompiler/frontend/cfg/ShaderCFG.cpp).

**Inferência:** esse critério é amplo demais para justificar sozinho a validade estrutural. Um bloco externo pode ser um trecho de saída intermediário, sem ser o merge/continue permitido pela estrutura SPIR-V.

**Documentação histórica:** o plano local identifica exatamente esse padrão em shaders RT. **A validar:** reproduzir com os shaders capturados e o emissor atual. Os compute RT examinados são descartados antes de alcançar esse problema, mas a reprodução não depende de RT: basta passar pelo CFG um shader RT capturado com a instrução BVH substituída por um stub (por exemplo, zerando os VGPRs de destino) e validar o SPIR-V com `spirv-val`.

**Dispatcher global**

**Código:** o fallback emite:

- Um loop principal.
- Um `OpSwitch` selecionado por um PC.
- Casos correspondentes aos blocos.
- Variáveis para phis e diversos valores usados entre blocos.
- Loads/stores para transportar esse estado.

Referência: `EmitDispatcherFunction` e seleção dos valores armazenados em [spirvEmitterProgram.cpp](../src/graphics/shader/recompiler/backend/spirv/spirvEmitterProgram.cpp).

**Inferência:** essa forma pode ampliar os intervalos de vida dos valores, dificultar otimizações e aumentar pressão de registradores e spills. As variáveis SPIR-V não correspondem automaticamente a acessos físicos à memória: o resultado depende das otimizações do driver.

O plano histórico registra um experimento com 128 blocos e 1.024 valores: aproximadamente **0,48 s** para a forma estruturada e **mais de 20 s** para o dispatcher, interrompido pelo limite do teste. Isso sustenta a hipótese arquitetural, mas **não é uma medição desta revisão nem uma previsão para todo shader RT**. [Experimento documentado](perf-research/raytracing-plan.md), [gerador das formas](perf-research/rt-experiments/dispatcher_shape_gen.cpp), [harness de pipeline](perf-research/rt-experiments/vk_pipeline_time.cpp).

**Branches artificiais e limites que alteram resultados**

Blocos sintéticos podem ser necessários para representar CFG válido. O problema aparece quando o roteamento transforma regiões extensas em estado artificial, duplicação ou seleção excessiva.

**Código:** o dispatcher possui limite padrão de **4.096 transições**. O comentário informa explicitamente que ele pode evitar um reset da GPU sacrificando o resultado. Esse limite não constitui emulação semanticamente correta. [Limite do dispatcher](../src/graphics/shader/recompiler/CodegenOptions.h).

Também existe otimização SPIR-V conservadora habilitada por padrão. Ela não substitui a correção do structurizer. Se a otimização/validação falha, o chamador registra aviso e conserva o módulo original; portanto, essa etapa não garante que todo módulo inválido seja bloqueado. [Otimizador](../src/graphics/shader/recompiler/backend/spirv/SpirvOptimizer.cpp), [tratamento da falha em CompileProgram](../src/graphics/shader/recompiler/ShaderRecompiler.cpp).

## 7 O que ainda é desconhecido sobre o BVH do PS5

É necessário distinguir **o formato aceito pela instrução AMD** das **estruturas usadas pela biblioteca de construção/traversal do jogo**.

**Documentação pública e pesquisa local:**

- Há formatos AMD com boxes FP16/FP32 e folhas de triângulos.
- Um teste de box pode retornar quatro ponteiros de filhos.
- O teste de triângulo pode retornar numeradores/denominador e outros campos, conforme o modo.
- O descritor influencia parâmetros como crescimento de boxes e ordenação.
- Os formatos de retorno não equivalem simplesmente a “distância e barycentrics”.

Referência: [pesquisa de ISA e descritores](perf-research/raytracing-plan.md).

**Existe uma divergência documental relevante.** O plano antigo presume fortemente RTIP 1.1 e questiona o formato comprimido. O catálogo posterior relata engenharia reversa de Psr, incluindo:

- Headers de hierarquia e listas de folhas.
- Nós de triângulos em fan.
- Registros de instância de 128 bytes.
- Boxes comprimidos de tipo 6, com expoentes compartilhados e filhos implícitos.
- Correções posteriores para estruturas compactadas.

Referência: entradas do protótipo Psr, da decodificação de tipo 6 e da correção do formato compactado no [catálogo de alterações](CHANGE-CATALOG.md).

**Esses relatos não autorizam tratar “tipo 6” como um significado universal.** É preciso verificar se o identificador pertence ao ponteiro entregue à instrução, ao formato Psr manipulado pelo shader ou a alguma representação intermediária.

Pontos ainda **a validar**:

| Questão | Por que importa |
| --- | --- |
| Layout efetivo por jogo/versão | Evita interpretar dados válidos usando o formato errado |
| Instâncias, transformações e máscaras | Necessários para reproduzir TLAS |
| Seleção e ordenação dos vértices | Afetam winding, barycentrics e identidade do hit |
| Formatos compactados e relocação | Afetam endereços e reutilização |
| Regras de empate e early exit | Podem tornar a ordem observável |
| Bits reservados/extensões Sony | Podem alterar comportamento não coberto por fontes PC |
| Momento de validade do BVH | Evita reconstruir a cena a partir de dados parciais |
| Escritas durante a execução | Podem inviabilizar um espelho estático para aquele trecho |

Os testes históricos com estruturas sintéticas ajudam, mas não resolvem essas questões para todos os jogos.

## 8 Viabilidade de hardware RT Vulkan

**Contrato Vulkan:** `VK_KHR_ray_query` permite consultas dentro dos estágios de shader existentes. Não exige transformar o emulador em um pipeline com raygen/hit/miss shaders. Isso torna compute com Ray Query um encaixe adequado para esse projeto. [Extensão Ray Query](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html).

Seriam necessários:

- Features `rayQuery` e `accelerationStructure`, além das dependências aplicáveis.
- Recursos `VkAccelerationStructureKHR`.
- Construção de BLAS/TLAS.
- Bindings e lifetime apropriados.
- Emissão de `SPV_KHR_ray_query`, tipos e operações `OpRayQuery*KHR`.
- Seleção de backend e cache compatíveis com as capacidades do dispositivo.

**Código:** a infraestrutura examinada exige buffer device address, mas não foi encontrada integração RT correspondente no código ativo. BDA é um pré-requisito útil; não habilita Ray Query por si só. [Inicialização Vulkan](../src/graphics/presentation/window/vulkanWindow.cpp).

**Reconstruir BLAS/TLAS é teoricamente viável.** É necessário extrair geometria ou AABBs e instâncias e pedir ao driver que construa novas estruturas. A representação interna Vulkan é opaca: não se pode simplesmente apontar um `VkAccelerationStructureKHR` para bytes do BVH PS5. [Estruturas Vulkan](https://github.khronos.org/Vulkan-Site/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html).

“Reconstrução no host” deve significar preferencialmente **gerenciamento pelo emulador**, com extração/conversão GPU e build GPU. Ler toda a hierarquia pela CPU a cada frame provavelmente introduziria um custo importante de sincronização.

## 9 Limitações dessa abordagem

A diferença de contrato é decisiva:

| Teste de nó PS5 | Ray Query Vulkan |
| --- | --- |
| Recebe um nó guest específico | Recebe uma acceleration structure |
| Expõe resultados do teste daquele nó | Expõe candidatos/resultados de traversal |
| Guest escolhe o próximo nó | Implementação conduz o traversal interno |
| Retorna ponteiros e campos no formato guest | Retorna identificadores e atributos Vulkan |
| Pode alimentar lógica arbitrária | Opera sob o modelo de consulta Vulkan |

**Inferência:** uma Ray Query não consegue reproduzir diretamente a lista de quatro filhos de um box guest ou o estado intermediário arbitrário esperado pelo shader.

A especificação também não garante ordem entre candidatos e admite diferenças numéricas; empates podem selecionar resultados diferentes. [Regras de traversal Vulkan](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html).

Consequentemente, a substituição precisa provar que o trecho guest depende apenas de um contrato de resultado compatível, e não de:

- Ordem de visitas.
- Contadores, atomics ou escritas por nó.
- Estado intermediário da pilha.
- Identidade do primeiro candidato.
- Resultados aritméticos específicos da instrução.
- Comunicação entre lanes durante o traversal.

Reconhecer o opcode é fácil. Demonstrar a equivalência do trecho que o usa é a parte difícil.

## 10 Alternativas possíveis

| Alternativa | Avaliação |
| --- | --- |
| Teste de nó software no shader traduzido | Melhor base de compatibilidade; preserva traversal guest |
| Intrinsic software compartilhado por módulo | Reduz expansão do IR; ganho GPU depende do driver |
| Traversal completo reconhecido e reemitido em compute | Permite otimizar sem depender de hardware RT |
| Ray Query com triângulos nativos | Maior exposição a diferenças numéricas e de aceitação |
| Ray Query com AABBs e interseção software das folhas | Primeiro candidato híbrido mais conservador |
| Emulação CPU | Útil como referência; inadequada como primeira opção de execução em tempo real |
| Backend específico de driver/AMD | Possível pesquisa, mas reduz portabilidade e exige contratos adicionais |

**Recomendação:** investigar primeiro o híbrido de AABBs:

```text
Ray Query percorre a estrutura Vulkan
  → fornece candidato AABB
  → shader localiza a folha guest
  → executa teste software compatível
  → aplica aceitação/máscaras do guest
  → registra resultado
```

A API permite candidatos AABB e interseções geradas pelo shader. [Contrato de candidatos](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/raytraversal.adoc).

**Limitação importante:** teste exato de folha não torna todo o resultado exato. Boxes ancestrais, transformações e ordem de traversal ainda podem divergir. Até encontrar um hit geometricamente correto que o traversal guest rejeitaria é uma diferença de emulação.

Outra opção intermediária é preservar traversal e transformações de instâncias em software e acelerar apenas a busca dentro de BLAS reconhecidas.

## 11 Riscos técnicos

Os riscos mais relevantes são:

- **Seleção incorreta do padrão:** substituir um loop que contém efeitos observáveis além da consulta.
- **Snapshot inconsistente:** combinar geometria antiga, instâncias novas e ponteiros realocados.
- **Invalidação incompleta:** ignorar escritas por BDA, cópias, aliases ou compactação.
- **Precisão:** diferenças em bordas, degenerados, NaNs, infinitos, zero com sinal, denormals e contração de operações.
- **Transformações:** alteração de `t` por normalização indevida da direção ou escala não uniforme.
- **Aceitação:** divergência em máscaras, orientação, alpha test e desempate.
- **Execução coletiva:** alterar EXEC, comportamento wave32/wave64, LDS ou operações de subgroup.
- **Lifetime:** reconstruir ou liberar AS/scratch ainda em uso.
- **Limites defensivos:** classificar resultados truncados como válidos.

Um cuidado adicional: recomputar a interseção software **depois** de receber um candidato de triângulo nativo não recupera um triângulo que o hardware já descartou. AABBs conservadoras reduzem esse problema, mas precisam de validação própria.

**Inferência:** fallback deve ser decidido antes dos efeitos observáveis da região, ou requerer um mecanismo explícito de reinício seguro. Não se deve executar parte do caminho hardware e depois repetir indiscriminadamente o trecho software.

## 12 Compatibilidade AMD e NVIDIA

| Família | Viabilidade esperada | Limite principal |
| --- | --- | --- |
| AMD RDNA2 | Hardware RT Vulkan viável em configurações com features necessárias | Proximidade de ISA não garante formato ou resultado idêntico |
| AMD RDNA3 | Mesmo backend KHR é viável | Validar precisão, compiler e custo por workload |
| AMD RDNA4 | Viável e promissor para workloads RT | Não presumir compatibilidade binária do BVH com PS5 |
| NVIDIA RTX | Viável via extensões KHR | Arquitetura distinta exige validação numérica e comportamental |

AMD documenta sua evolução de aceleradores RT; NVIDIA oferece as extensões Vulkan para acesso à aceleração RTX. Isso sustenta a viabilidade de um backend portátil, **não uma equivalência automática com PS5**. [AMD RDNA4](https://ir.amd.com/news-events/press-releases/detail/1238/amd-unveils-next-generation-amd-rdna-4-architecture-with-the-launch-of-amd-radeon-rx-9000-series-graphics-cards), [NVIDIA Vulkan](https://developer.nvidia.com/Vulkan).

A seleção deve consultar features e limites reais do dispositivo/driver. Não deve depender apenas de “AMD”, “RTX” ou da versão Vulkan anunciada.

O fallback software ainda depende das capacidades gerais exigidas pelo emulador; não significa compatibilidade com qualquer GPU.

## 13 Impacto esperado em performance

**Não há evidência suficiente para estimar FPS com credibilidade.** A comparação correta é RT software funcional contra RT acelerado equivalente, não contra RT descartado.

O ganho líquido depende de:

```text
custo software evitado
  − conversão
  − build/update de AS
  − sincronização adicional
  − traversal/callbacks restantes
```

| Aspecto | Efeito potencial |
| --- | --- |
| GPU | Redução relevante do trabalho de traversal em cenas reutilizáveis |
| CPU | Menos IR/CFG se a região inteira for substituída; mais gerenciamento de AS |
| Compilação | Pode cair, mas Ray Query também introduz estado e código do driver |
| VRAM | Aumenta com espelho AS, geometria/AABBs, remapeamento e scratch |
| Frame pacing | Pode melhorar com caches; piorar com rebuilds ou readbacks |
| Estabilidade | Depende de snapshots, invalidação, sincronização e equivalência |

**Cache BLAS/TLAS é viável e provavelmente essencial.** A chave precisa considerar identidade da alocação, geração, conteúdo, layout, dependências e configuração de build. Ponteiro guest isolado não é suficiente.

Uma política futura plausível:

- Geometria estática: reutilizar BLAS.
- Mudança apenas de instâncias: atualizar/reconstruir TLAS.
- Deformação compatível: avaliar update.
- Mudança de topologia/quantidade: reconstruir quando exigido.
- Memória realocada ou conteúdo desconhecido: invalidar.

Updates Vulkan têm restrições: não são uma operação genérica para qualquer alteração de geometria. [Regras de atualização](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html).

A sincronização necessária inclui conversão → build, BLAS → TLAS e build → shader consumidor. Para Ray Query em compute, o consumidor é o estágio compute com acesso de leitura de AS. Não é necessário impor uma espera CPU a cada etapa. [Guia Khronos de RT](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html).

## 14 Arquitetura recomendada

A proposta original é válida **se distinguir dois níveis de intrinsic**:

```text
Shader PS5
  → Decoder
  → IR fiel à ISA
      → GuestBvhNodeIntersect
          → implementação software no shader
          → preservação do traversal original

  → análise de regiões de traversal no IR
      → região reconhecida e validada
          → GuestTraceRay / GuestTraceOcclusion
              → traversal software equivalente
              → Ray Query + AABBs + folhas software
              → Ray Query + triângulos, quando validado
```

Em paralelo:

```text
Memória e builders guest
  → identificação de estruturas e versões
  → conversão GPU
  → espelho BLAS/TLAS
  → remapeamento de IDs Vulkan para objetos guest
  → sincronização e gerenciamento de lifetime
```

Os nomes acima são conceituais; não existem atualmente.

**O intrinsic de baixo nível** deve representar o contrato da instrução: descritor, ponteiro, parâmetros do raio, modo e máscara de execução. Deve modelar leitura de memória; não pode ser tratado como operação matemática independente de escritas no BVH.

**O intrinsic de alto nível** deve representar o resultado observável de uma região cuja equivalência foi demonstrada: closest hit, oclusão ou outro contrato específico.

Essa separação permite:

- Suporte geral sem depender de reconhecer bibliotecas.
- Aceleração progressiva por padrões de traversal.
- Comparação diferencial entre implementações.
- Fallback conservador quando o formato, snapshot ou comportamento não for conhecido.
- Backend KHR compartilhado entre AMD e NVIDIA.

**A seleção hardware/software não deve ocorrer somente na tradução de `IMAGE_BVH_*`.** Ela depende também da região de programa e da cena disponível.

Para compatibilidade estrita, regiões não comprovadas permanecem no caminho software. O backend híbrido deve ser uma otimização condicionada por evidências, não uma aproximação silenciosa.

## 15 Ordem recomendada de investigação futura

0. **Estabilidade (feito nesta revisão).** Shaders não compute com BVH são descartados em vez de abortar o emulador.

1. **Consolidar as evidências das branches RT.** Recuperar e revisar os protótipos registrados, distinguindo código, testes sintéticos e resultados em jogo.

2. **Resolver o contrato do BVH real.** Capturar descritores, raízes, nós, instâncias e versões coerentes; reconciliar RTIP/Psr e estruturas compactadas. Os passos 1 e 2 são os bloqueadores reais e devem ser um único entregável: um dump dos nós BVH reais de um jogo com o layout decodificado. Sem ele, o backend software (passo 6) não avança.

3. **Mapear os produtores da estrutura.** Validar construção, refit, compactação, callbacks e escritas antes de atribuir erros ao traversal.

4. **Estabelecer uma referência de semântica.** Comparar testes de nó e regiões completas, com dados independentes das rotinas usadas para gerar fixtures.

5. **Validar CFG/SPIR-V dos shaders reais.** Reproduzir saídas múltiplas, merges, branches artificiais e fallback; separar validade de custo de compilação.

6. **Obter um caminho software funcional e observável.** Preservar traversal guest; registrar quando limites ou dados inválidos comprometem resultados.

7. **Medir o custo completo.** Separar compilação fria, execução GPU, memória, readbacks e esperas, mantendo cena e efeitos equivalentes.

8. **Provar invalidação e snapshots.** Cobrir escritas CPU/GPU, aliases, reutilização de endereços e frames em voo.

9. **Prototipar hardware em uma região simples** (bloqueado até o passo 6 fornecer uma referência de correção). Preferir geometria estática e resultado pouco dependente da ordem, como uma consulta de oclusão bem delimitada.

10. **Comparar três caminhos.** Software fiel, AABBs híbridas e triângulos nativos, incluindo raios em bordas e casos degenerados.

11. **Validar em AMD e NVIDIA.** Medir resultados e desempenho por driver, incluindo pressão de registradores, scratch e consumo de VRAM.

12. **Ampliar somente os padrões comprovados.** Manter fallback software para o restante.

**Resposta à questão central:** hardware RT Vulkan pode acelerar regiões de Ray Tracing emuladas. **A preservação geral da semântica não é garantida por Ray Query e ainda não foi demonstrada para este emulador.** A solução mais segura é manter a emulação software da instrução como base e acrescentar aceleração de traversal completo, validada por padrão e por estrutura. O primeiro backend hardware a investigar é **Ray Query sobre AABBs, com teste e aceitação das folhas em software**, acompanhado de um espelho BLAS/TLAS corretamente versionado e sincronizado.
