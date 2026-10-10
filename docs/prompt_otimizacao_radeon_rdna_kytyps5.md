# Prompt — Otimização Radeon/RDNA no KytyPS5

Atue como um engenheiro sênior especializado em **C++ de alta performance, Vulkan e arquitetura AMD Radeon/RDNA (RDNA2, RDNA3 e RDNA4)**.

Analise e otimize o renderer/emulador KytyPS5 com foco em **FPS alto, baixa latência, fluidez, estabilidade e qualidade gráfica**, aproveitando CPU e GPU de forma equilibrada. Use boas práticas de C++ moderno e Vulkan, profiling orientado por dados e mudanças pequenas, seguras e mensuráveis.

## Objetivos

- Reduzir `Thread_Gpu`, CPU overhead, stalls e sincronizações CPU↔GPU.
- Aumentar a utilização real da GPU sem criar contenção no CPU.
- Reduzir `DrawPrep`, `DrawRun`, `vkQueueSubmit`, readbacks e waits.
- Aumentar continuidade de draw batches e diminuir serial fallbacks.
- Reduzir pressão de VGPR/SGPR, spills, scratch e tamanho do SPIR-V.
- Preservar uniformidade de dados para favorecer scalar loads em Radeon.
- Manter fidelidade visual e comportamento correto do jogo.
- Evitar regressões, artefatos, `device lost`, race conditions e memory leaks.
- Priorizar ganhos reais de frame time e FPS, não apenas microbenchmarks.

## Prioridades de implementação

### 1. Read-only buffers por binding

- Detectar leitura/escrita por recurso individual.
- Aplicar `NonWritable` somente onde for semanticamente seguro.
- Preservar coherent/polling buffers.
- Favorecer scalar loads e melhor codegen AMD.

### 2. Uniformity analysis

- Propagar `Uniform / Divergent / Unknown` no IR.
- Preservar valores oriundos de SGPR/constants como uniformes.
- Evitar transformar acessos uniformes em operações vetoriais por lane.
- Facilitar uso de SALU/SGPR pelo driver AMD.

### 3. Redução de pressão de registradores

- Encurtar live ranges.
- Eliminar temporários, bitcasts e conversões redundantes.
- Aplicar constant folding, DCE e CSE seguros.
- Reduzir duplicação de address/BDA calculations.
- Medir VGPR, SGPR, spills, scratch e occupancy.

### 4. Indirect draws GPU-native

- Evitar readback de argumentos gerados pela GPU.
- Manter indirect mesh/draws na GPU sempre que possível.
- Eliminar `SideReadbackState::Wait`, waits equivalentes e round-trips GPU→CPU→GPU.

### 5. DrawPrep / DrawRun

- Aumentar `continued %`.
- Reduzir serial fallbacks.
- Manter batches vivos através de operações que não exigem quebra real.
- Retirar trabalho raro ou caro do hot path.
- Evitar locks, alocações e cópias por draw.

### 6. Descriptors / bindings

- Reutilizar bindings e descriptors.
- Prefetch de estruturas de lookup quando comprovadamente útil.
- Evitar atualizações redundantes.
- Favorecer tabelas persistentes/indexadas em vez de rebinding por draw.

### 7. Barriers e sincronização

- Usar dependências Vulkan precisas.
- Rastrear recurso, range, stage, access e layout.
- Evitar `ALL_COMMANDS`, full barriers e waits globais quando não forem necessários.
- Tratar RAW/WAR/WAW somente em ranges realmente sobrepostos.
- Nunca sacrificar correção por remoção agressiva de sincronização.

### 8. SPIR-V / shaders Radeon

- Reduzir tamanho do módulo e duplicação de funções.
- Evitar inlining excessivo em shaders grandes.
- Compartilhar funções RT/BVH quando reduzir código e register pressure.
- Preservar informações que permitam bom codegen no driver AMD.
- Comparar SPIR-V, ISA AMD, compile time e frame time.

### 9. Wave32 / Wave64

- Não forçar globalmente.
- Testar via capabilities/subgroup controls quando suportado.
- Selecionar por shader somente quando benchmark comprovar ganho.

### 10. Uploads e memória

- Avaliar caminho otimizado para BAR/ReBAR/SAM apenas quando adequado.
- Usar para pequenos dados, constantes, parâmetros e rings.
- Evitar colocar recursos grandes em memória CPU-visible sem necessidade.
- Minimizar cópias e movimentação entre RAM e VRAM.

### 11. Async compute

- Usar somente onde existir overlap real e dependências claras.
- Não mover trabalho para outra queue apenas por arquitetura.
- Validar com profiling se houve ganho.

### 12. Ray Tracing

- Primeiro otimizar o RT em software: SPIR-V, VGPR, uniformidade, BVH traversal e early exits.
- Depois estudar hardware RT/RayQuery como etapa separada.
- Não implementar uma arquitetura complexa de RT sem prova clara de benefício.

## Regras de engenharia

- Use C++ moderno, RAII, ownership explícito e estruturas cache-friendly.
- Evite alocações no hot path.
- Evite abstrações, templates ou sistemas genéricos sem ganho comprovado.
- Evite overengineering.
- Não crie um backend AMD separado se capabilities e fast paths forem suficientes.
- Preserve o caminho genérico e introduza otimizações Radeon de forma isolável.
- Toda otimização deve possuir fallback simples ou feature flag quando houver risco.
- Evite locks globais, mutexes frequentes e contenção entre threads.
- Prefira estruturas lock-free ou thread-local somente quando a complexidade for justificada por profiling.
- Minimize branches imprevisíveis em hot paths.
- Minimize virtual calls, RTTI e indireções desnecessárias no caminho por draw.
- Evite cópias de estruturas grandes.
- Use `span`, referências e views quando adequado.
- Não aumentar significativamente complexidade de manutenção por ganhos marginais.
- Não alterar precisão gráfica, formatos, sincronização ou ordenação sem validação visual e funcional.

## Uso de skills e conhecimento especializado

Use skills/agentes especializados em:

- **C++ performance**
- **Vulkan**
- **SPIR-V**
- **AMD RDNA / Radeon**
- **GPU profiling**
- **multithreading**
- **memory/cache optimization**
- **shader compiler / compiler IR**

Para cada mudança, valide a decisão considerando C++, Vulkan e arquitetura de GPU em conjunto. Não aplique padrões genéricos sem verificar o fluxo real do KytyPS5.

## Metodologia obrigatória

Antes de implementar:

1. Identifique o gargalo real.
2. Localize o hot path.
3. Meça baseline.
4. Explique a causa.
5. Proponha a menor mudança possível.
6. Estime risco de regressão.
7. Implemente.
8. Rode testes.
9. Faça benchmark A/B.
10. Compare qualidade gráfica e estabilidade.

Nunca faça otimização baseada apenas em hipótese quando for possível medir.

## Métricas obrigatórias

Colete antes e depois:

- FPS médio.
- 1% low.
- frame time médio, p95 e p99.
- `Thread_Gpu` ms/frame.
- CPU usage por thread crítica.
- GPU utilization.
- GPU frame time.
- `DrawPrep` committed.
- serial fallbacks.
- `DrawRun eligible`.
- `key matches`.
- `continued %`.
- `waited`.
- `after CP stop`.
- readbacks/frame.
- waits CPU↔GPU.
- `vkQueueSubmit` tempo/frame.
- número de submits/frame.
- barriers/frame.
- pipeline creations.
- shader compile time.
- SPIR-V instruction count.
- SPIR-V word count.
- VGPR.
- SGPR.
- spills.
- scratch usage.
- occupancy/waves.
- VRAM usage.
- RAM↔VRAM transfers.
- page faults/write faults quando relevantes.

## Ferramentas de profiling

Quando disponíveis, use:

- Radeon GPU Profiler (RGP).
- Radeon Memory Visualizer (RMV).
- Radeon GPU Detective (RGD).
- RenderDoc.
- Vulkan validation layers.
- Tracy.
- ETW/Windows Performance Recorder.
- profiler de CPU.
- dumps de SPIR-V e ISA AMD.
- métricas internas do KytyPS5.

Não deixe ferramentas de diagnóstico caras ativas em builds finais.

## Critérios de aceitação

Uma otimização só deve permanecer se cumprir pelo menos um destes pontos sem regressão relevante:

- reduzir frame time;
- reduzir `Thread_Gpu`;
- aumentar FPS;
- melhorar 1% low;
- reduzir stalls/readbacks;
- reduzir VGPR/spills;
- reduzir compile time;
- aumentar `DrawRun continued %`;
- reduzir submissões/barriers;
- melhorar estabilidade.

E deve manter:

- mesma imagem ou diferença visual comprovadamente aceitável;
- ausência de novos artefatos;
- ausência de crashes;
- ausência de `VK_ERROR_DEVICE_LOST`;
- ausência de race conditions;
- ausência de memory leaks;
- comportamento determinístico quando exigido.

## Prioridade de trabalho

Ataque uma frente por vez nesta ordem:

1. read-only por binding;
2. uniformity/scalarização;
3. indirect GPU-native;
4. DrawPrep/DrawRun;
5. binding/descriptors;
6. redução de VGPR/SPIR-V;
7. barriers e sincronização;
8. wave size;
9. uploads/ReBAR;
10. async compute;
11. RT software;
12. hardware RT.

Não abra várias frentes ao mesmo tempo.

## Resultado esperado

Para cada alteração, responda de forma objetiva com:

- **Gargalo**
- **Causa**
- **Arquivo/função afetada**
- **Mudança proposta**
- **Por que beneficia Radeon**
- **Risco**
- **Métrica antes**
- **Métrica depois**
- **Impacto visual**
- **Manter ou reverter**

Priorize sempre **performance real, baixa latência, fluidez e qualidade de imagem**, com código simples, seguro e sustentável.
