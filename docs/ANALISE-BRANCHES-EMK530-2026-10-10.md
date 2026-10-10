# Branches de EmK530/KytyPS5 — análise de utilidade (2026-10-10)

## Resultado

A melhor candidata a experimento de desempenho é a submissão periódica de draws da branch `perf/renderer-optimizations`. Ela trata um caso diferente do flush ocioso que já existe aqui: submete trabalho enquanto a CPU continua gravando uma cadeia longa, em vez de esperar a GPU ficar sem trabalho pendente. A branch `perf/spirv-optimizations` também merece comparação seletiva por conter mudanças de compatibilidade de shaders e descritores bindless que tocam áreas em trabalho no nosso fork. Nenhuma das duas justifica um merge integral.

Esta leitura cobre as sete branches visíveis em [EmK530/KytyPS5/branches](https://github.com/EmK530/KytyPS5/branches), conforme observadas em 2026-10-10. A referência simbólica `HEAD` do clone aponta para `perf/renderer-optimizations`, embora a atividade mais recente esteja em `perf/spirv-optimizations`.

## Branches e prioridade

| Branch | Ponta observada | O que pode interessar ao fork | Prioridade |
| --- | --- | --- | --- |
| [`perf/renderer-optimizations`](https://github.com/EmK530/KytyPS5/tree/perf/renderer-optimizations) | `41f7794` — 2026-09-22 | Submissão periódica de draws; comparar o intervalo configurável com o scheduler local. | Alta: experimento isolado |
| [`perf/spirv-optimizations`](https://github.com/EmK530/KytyPS5/tree/perf/spirv-optimizations) | `d2413fc` — 2026-10-07 | Correções de shaders, seletores e tabelas de imagens bindless; há sobreposição com trabalho local ativo. | Alta: revisar commits específicos |
| [`fix/neptunia-saves`](https://github.com/EmK530/KytyPS5/tree/fix/neptunia-saves) | `54d88ce` — 2026-09-28 | CP932 e correção de loop em SaveDataDialog. O ancestral correspondente já está no HEAD local (`05f92b64`). | Baixa: provável duplicata |
| [`shader/neptunia-fixes`](https://github.com/EmK530/KytyPS5/tree/shader/neptunia-fixes) | `c9fb9fe` — 2026-09-27 | Ajustes de sampler, textura e canais para shaders de Neptunia. Revisar se houver regressão ou jogo-alvo afetado. | Condicional |
| [`fix/criware-audio`](https://github.com/EmK530/KytyPS5/tree/fix/criware-audio) | `096401e` — 2026-09-26 | Captura PCM no momento da configuração dos atributos quando portas AudioOut2 compartilham buffer. O ancestral `313b60e9` já consta no HEAD local. | Baixa: verificar apenas sintoma específico |
| [`main`](https://github.com/EmK530/KytyPS5/tree/main) | `f7ae6e0` — 2026-09-25 | Base de referência mais antiga que as branches com atividade posterior. | Baixa |
| [`experiment/gc-relaxing`](https://github.com/EmK530/KytyPS5/tree/experiment/gc-relaxing) | `564ad0e` — 2026-09-18 | Experimento antigo de política de garbage collection. | Baixa: só com evidência de pressão de memória/GC |

## Flush periódico de draws

O commit [`097e7c2`](https://github.com/EmK530/KytyPS5/commit/097e7c2) introduz `KYTY_DRAW_FLUSH_INTERVAL` e chama `CompleteDraw()` após um número configurável de draws. A justificativa é evitar que uma sequência longa fique gravada no host sem submissão, deixando a GPU sem trabalho. O commit relata redução do tempo da thread principal em `MasterSemaphore::Wait` para menos de 10% no caminho de jogo medido pelo autor; esse resultado é específico àquele teste e ainda não foi reproduzido neste fork. O commit [`41f7794`](https://github.com/EmK530/KytyPS5/commit/41f7794) altera o valor padrão de 16 para 256, sinal de que o intervalo depende do workload.

O fork já tem `KYTY_IDLE_FLUSH_DRAWS` (padrão 8) em `src/graphics/guest_gpu/graphicsRun.cpp`. `MaybeFlushIdleGpu()` faz submissões antecipadas depois que a GPU consumiu o trabalho já enviado. O flush periódico da branch doadora pode agir antes disso, enquanto a GPU ainda está ocupada e a CPU continua gravando; portanto, não é a mesma política e vale um A/B controlado.

Experimento sugerido: comparar o comportamento atual com intervalos 16 e 256 em uma cena reproduzível, alternando as configurações em ordem e mantendo o mesmo tempo por janela. Registrar FPS e tempo de quadro, tempo em `MasterSemaphore::Wait`, quantidade de submissões e sinais de ociosidade da GPU. Tratar 16 e 256 como candidatos, não como valores presumidamente melhores; só adotar uma alteração após ganho repetível sem regressão em outros workloads.

A branch também inclui otimizações de limpeza de SRT e redução de `CompiledOp`. No fork, porém, já existem receitas/tapes/memoização de SRT e outras otimizações de estado. Compare semântica, invalidação e contabilidade antes de portar essas partes.

## Shaders e descritores bindless

`perf/spirv-optimizations` é a branch mais nova, mas seu escopo é amplo: além de SPIR-V, altera descritores, gráficos, kernel, launcher e áudio. Alguns commits são referências potencialmente úteis para o trabalho atual de recursos e Yōtei:

- [`b929bbe3`](https://github.com/EmK530/KytyPS5/commit/b929bbe3): compatibilidade ampla com tabelas de imagens.
- [`ae1e9262`](https://github.com/EmK530/KytyPS5/commit/ae1e9262): imagens bindless mistas por arrays contíguos.
- [`cd066010`](https://github.com/EmK530/KytyPS5/commit/cd066010): substitui cadeias de seletores na CPU por acesso direto à tabela de imagens.
- [`c23de621`](https://github.com/EmK530/KytyPS5/commit/c23de621): tabelas de seletores aninhadas e offsets escalares exatos.
- [`9caec9f2`](https://github.com/EmK530/KytyPS5/commit/9caec9f2): coerência DLC para RDNA2.

Esses commits tocam arquivos que também têm mudanças locais, incluindo `ResourceMaterialization.cpp`, `ResourceTracking.cpp`, `BindingLayout.cpp`, `spirvEmitterImage.cpp`, emissores de memória e testes. A sequência segura é comparar cada diff com o estado local depois de reconciliar as mudanças em andamento; um merge da branch inteira misturaria alterações independentes e criaria risco de sobrescrever trabalho. O fork já implementa, entre outros itens, `V_MIN3_U16` e operações `BUFFER_LOAD/STORE_FORMAT_D16_X`, então confirme se cada commit é realmente novo antes de considerar portá-lo.

## Sobreposições e referências de RT

As correções de Neptunia para CP932/SaveDataDialog e a captura de PCM do CRIware já têm commits ancestrais no HEAD local. As branches podem conter refinamentos posteriores, mas não são uma fonte de funcionalidade inteiramente nova para o fork.

O fork também já aceita BVH64 e A16 na interseção por software, conforme [`BVH64-A16-2026-10-10.md`](BVH64-A16-2026-10-10.md). O commit [`dbf66b7f`](https://github.com/EmK530/KytyPS5/commit/dbf66b7f) de BVH64 por software é útil como referência comparativa, não como substituto automático da implementação local. O protótipo de hardware RT continua sendo uma linha separada; uma operação BVH guest não deve ser trocada por Vulkan RayQuery sem comprovar equivalência de semântica.

## Próximos passos

1. Priorizar um A/B do flush periódico com os valores 16 e 256, medindo também esperas em `MasterSemaphore` e submissões.
2. Revisar isoladamente os commits bindless/SPIR-V citados, preservando e testando as alterações locais nos mesmos arquivos.
3. Consultar as branches de Neptunia e CRIware apenas se aparecer um caso reproduzível que indique diferença em relação aos ancestrais já integrados.
4. Deixar `experiment/gc-relaxing` para depois, a menos que profiling indique pressão de GC ou memória.
