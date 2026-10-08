# F1 — Cópia de uploads BDA no worker

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** tirar a cópia guest→staging dos uploads coletados pelo passe BDA do Thread_Gpu, preservando a ordem das escritas do CP.

**Architecture:** reutilizar o StagingCopier e sua SubmitDependency. Rastrear fontes por faixa e ticket de cópia; escritas do CP aguardam apenas tickets conflitantes. Coleta, proteção, hot pages, shadows, revisões e gravação Vulkan permanecem no proprietário atual.

**Tech Stack:** C++20, Vulkan/RADV, CMake/CTest.

**Spec:** [arquitetura, seções 1b, 4.1, 9.8 e 10](../../ARQUITETURA-ESCRITAS-RUNTIME-2026-10-08.md).
**Diagnóstico:** [F0](../../DIAGNOSTICO-F0-COERENCIA-2026-10-08.md).

## Restrições e decisões

- Proprietário: Codex / F1. Worktree existente `/home/jonathanbraga/kyty-coherence`, branch `cpu-coherence-service`.
- F1 começa em `FinishBdaBatchedUpload`, depois da proteção. `RunBdaPass`, `pageManager.*` e UFFD ficam para a F3 do Claude.
- `KYTY_COHERENCE_COPY=0|1|verify`, live, desligado por padrão. Desligar não elimina guards de jobs já admitidos.
- Preservar batch protect, incremental, dirty-log, hot pages, CP sequencer/recorder, DrawPrep, shaders e program cache.
- Bindings comuns de leitura pertencem à F2. A F1 copia no worker os runs guest do passe BDA; bytes de hot shadows já materializados continuam estáveis no staging.
- Reutilizar o worker e o slot 0 de SubmitDependency do StagingCopier evita outro worker e outro slot. Se texture staging estiver desligado, criar o worker de CPU sem ativar o caminho de texturas.
- Uma thread de cópia FIFO permite tickets com conclusão contígua. A F1 não adiciona um segundo worker.
- Copiar por aliases somente com lifetime garantido pelo drain anterior a unmap e pela dependência do submit; casos sem alias continuam síncronos.
- Flush de memória não coerente ocorre depois das cópias. Slots do staging não são reutilizados antes do tick consumidor.
- Escritas não ordenadas do guest preservam o contrato atual de redirty; os guards cobrem as escritas ordenadas do emulador.
- A/B usa a RX 9070 XT com cena parada e mesmo trabalho. Ganhos do F0 são estimativas, sem medição nova.
- Commit local autorizado pelo usuário após a validação; integração e A/B seguem em etapas próprias.

## Review Focus

1. Sobrescrever uma fonte pelo CP enquanto o worker ainda a lê deve preservar os bytes do upload anterior.
2. Desligar o switch com jobs em voo mantém os guards e as dependências daqueles jobs.
3. Texture async staging desligado, DMA disponível e memória não coerente preservam comportamento e ordem.
4. Unmap, reuso de staging e shutdown não invalidam pointers usados por um job.
5. Fontes sobrepostas e faixas independentes aguardam o ticket correto, sem drain global.

## Task 1: dependências de fontes por faixa

**Files:** criar `src/graphics/host_gpu/sourceCopyTracker.h`, `tests/SourceCopyTrackerTests.cpp`; registrar alvo e CTest em `CMakeLists.txt`.

**Interfaces:** `SourceCopyTracker(std::atomic<uint64_t>& completed)`; `Track(address,size,value)`; `PendingValue(address,size)`; `WaitRange(address,size)` retornando o ticket aguardado ou zero. O worker publica conclusão FIFO no atomic existente, com release e notify; o tracker consulta com acquire e reutiliza WriteTickMap.

- [x] Escrever testes de faixas independentes, sobreposição parcial, conclusão de A mantendo B, limites de endereço e overwrite da fonte após a cópia. O teste concorrente controla o início da cópia por semáforo e verifica os bytes preservados.
- [x] Verificar RED pela ausência da interface; registrar o resultado.
- [x] Implementar tracker com fast path sem lock quando não há ticket pendente, consulta protegida e espera fora do lock.
- [x] Rodar `source_copy_tracker_tests` e verificar GREEN.

## Task 2: compartilhar o worker de staging

**Files:** `renderer/image/stagingCopier.*`, `renderer/cache/textureCache.*`.

**Interfaces:** ampliar `StagingCopier::Range` com fonte opcional de backing; oferecer `BeforeEmulatorWrite(address,size)`; expor criação/acesso ao copier pelo TextureCache. Jobs de buffers registram suas fontes; jobs de textura existentes preservam seu caminho de leitura.

- [x] Acrescentar cobertura da dependência de fontes ao enfileirar e concluir jobs.
- [x] Copiar fontes de backing no worker e flush somente após terminar os bytes.
- [x] Preservar o desligamento de texture async staging mesmo quando a F1 criar o worker.
- [x] Confirmar drain e detach antes de destruir copier e staging.

## Task 3: uploads BDA e guards das escritas do CP

**Files:** `renderer/cache/bufferCache.*`, `renderer/cache/uploadDma.h` (endereço guest no metadado de cópia), `renderer/renderContext.cpp`, `guest_gpu/graphicsRun.cpp`, `common/profiler.*`.

**Interfaces:** `BufferCache::BeforeEmulatorWrite(address,size)`; `KYTY_COHERENCE_COPY`; contadores de bytes/jobs, guards/esperas e divergências de verify. Coleta e proteção atuais antecedem Enqueue. SubmitDependency existente antecede vkQueueSubmit.

- [x] Enfileirar os runs elegíveis de FinishBdaBatchedUpload; preservar fallback para aliases indisponíveis e temporários.
- [x] Registrar guards antes de DUMP_CONST_RAM, WRITE_DATA CPU, reference clock, labels imediatos/adiados, GDS, oclusão sintética, flip e PrepareHostBackingWrite.
- [x] Preservar bypass de WRITE_DATA que já foi emitido na GPU: ele não modifica uma fonte no backing da CPU.
- [x] Implementar verify com diagnóstico que distinga redirty concorrente do guest de divergência sem mutação registrada.
- [x] Confirmar que mudança live para 0 conserva os guards pendentes.

## Task 4: prova pelo caminho real e diagnóstico F0

**Files:** `tests/ShaderCoherenceCopyTests.inc`, `tests/ShaderRecompilerComputeTests.cpp`, `CMakeLists.txt`; atualizar este plano com os resultados e responder ao diagnóstico F0.

- [x] Exercitar BufferCache e o worker reais: upload anterior + escrita ordenada do CP + upload posterior devem produzir versões GPU distintas.
- [x] Exercitar submit com fontes pendentes, texture staging 0/1, modo 1/verify, fallback e teardown.
- [x] Rodar regressões focadas de coalescing, BDA new-buffer/epoch, scheduler/recorder e memória conforme o risco alterado.
- [x] Validar os números históricos do F0 e identificar qual trace corresponde ao perf-base.data; separar amostras de ciclos de tempo medido.
- [x] Preparar binário de comparação, controles live e contadores exportados.
- [ ] Medir FPS e perf/FaultMap na mesma cena, quando o usuário iniciar a próxima sessão (ficou para depois de dormir).
- [x] Revisar a branch inteira e corrigir riscos concretos encontrados.

## Registro da execução

- Task 1 RED: build falhou pela interface ausente em `SourceCopyTrackerTests.cpp:1` (`/tmp/kyty-coherence-f1-tracker-red.log`).
- Task 1 GREEN: `ctest -R '^source_copy_tracker$'`, 1/1 passou em 0,05 s.
- O tracker registra todas as fontes de um job sob um único lock. Só poda o mapa quando a conclusão avança; fonte pronta consulta apenas atomics.
- Decisão de implementação: limite inicial live de 16 KiB (`KYTY_COHERENCE_COPY_MIN_KB`), para medir custo de jobs pequenos. Não é uma recomendação de desempenho já medida.
- Uploads enviados à F1 bypassam UploadDma: o worker DMA não possui a dependência do copier. Uploads fora da F1 mantêm a rota anterior.
- Auditoria acrescentou reescritas de timestamps GPU em `eopTimestamps.*`; a ligação com BufferCache ocorre depois da construção das caches.
- Verify captura os bytes observados pelo worker em memória host normal e compara o readback nativo. A mudança do snapshot de admissão é classificada por FaultMutationEpoch; sem tracking de mutações a classificação é conservadora.
- Build próprio: fonte no worktree, saída em `/home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1`; Clang 22, Release, dependências locais com as revisões fixadas pelo projeto.

## Verificação final e revisão

- Build Release final com Clang 22: exit 0; logs em _Build/coherence-f1/validation/ no workspace principal.
- CPU: 6/6 passaram em 0,50 s. GPU/regressões: 19/19 passaram em 3,15 s, incluindo a matriz F1 4/4; total 25/25.
- Alias RED documentado; canonical backing corrigiu bypass por outra VA e por write spanning mappings reversos.
- Lease RAII protege publicações e admissão desde antes do primeiro job; teste conserva espera por B admitido enquanto o writer aguardava A. Espera fora do gate, seguida de reconsulta.
- Runs F1 reutilizam vetores de um pool limitado a 64 × até4096 ranges (~8 MiB). Native test confirma reutilização. Reuse e allocation são métricas independentes quando a capacidade aumenta.
- Consultas de guard têm contador/tempo separados das esperas. HangTrace acrescenta 13 colunas mem_coherence_*; Profiler mantém também as durações.
- Revisões independentes verificaram proteção de fontes, ordem de locks, lifetime, pooling e wakeup; nenhum finding concreto remanescente no escopo revisado.
- Resposta ao F0 salva em docs/RESPOSTA-F0-F1-COERENCIA-2026-10-08.md. Corrige a classificação heurística “kernel” e liga o perf histórico à execução trace-base, sem atribuir a janela A/B posterior.
- Novo resultado da fase 0 runtime do Claude registrado na resposta; essa frente permanece no worktree kyty-bda-writes.
- A/B pronto em _Build/coherence-f1/run-crash/start.sh. O usuário pediu para dormir: sem sessão nova do jogo; nenhum ganho de FPS da F1 foi declarado.
