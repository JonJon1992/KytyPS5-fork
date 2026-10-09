# F3: proteção de uploads no worker

> Execução: superpowers:executing-plans, worktree cpu-coherence-service.
> Plano solicitado pelo usuário; continuar diretamente até implementação e verificação.

**Goal:** tirar ApplyProtectBatch do Thread_Gpu nos passes BDA com cópias F1.
**Architecture:** batch move-only reutilizável do PageManager; job de proteção na
fila FIFO do StagingCopier, antes das cópias. Submit espera o mesmo valor monotônico.
**Tech Stack:** C++, PageManager, StagingCopier e wrappers Vulkan existentes.
**Spec:** docs/ARQUITETURA-ESCRITAS-RUNTIME-2026-10-08.md, seções 1b e 9.

## Restrições

- Switch live KYTY_COHERENCE_PROTECT=1, desligado por padrão.
- Read-watch/NoAccess, hot snapshots, modos verify e escopos aninhados seguem síncronos.
- Apply reavalia os watcher counts sob os locks atuais, sem capturar permissões.
- Callers/jobs param antes de destruir PageManager ou seu backing.
- Mesma thread, SubmitDependency, guards físicos e leases de publicação da F1.
- Armazenamento das batches volta a pool limitado; nenhuma thread extra.
- Fallback sem alias/reserva temporária espera só o ticket de proteção antes de ler.
- Capturar o modo por passe; mudanças live não abandonam jobs pendentes.

## Foco da revisão

Unwatch após coleta; read-watch novo; cancelamento e move assignment; fallback
inline antes de Apply; lifetime dos jobs durante unmap/shutdown.

## Task 1: transferir batches

Arquivos: pageManager.h/.cpp; tests/PageManagerTests.cpp.
Interfaces: PageManager::ProtectBatch move-only com Apply(), Empty(), Size(), Capacity().
DeferProtectScope::DetachTo(ProtectBatch&) recusa nested/destino ocupado.
O destrutor da batch aplica spans pendentes; o escopo destacado encerra seu depth.

- [x] TestTransferredProtectBatch: RW até Apply em outra thread, depois RO;
  unwatch, read-watch novo, cancelamento, move assignment e nested fallback.
- [x] RED: build page_manager_tests rejeitou a API ausente (f3-red-build.log).
- [x] Compartilhar ApplyProtectBatch(vector&) entre batch transferida e TLS.
- [x] Build e page_manager_tests completos aprovados (f3-page-tests.log, 34,5 s).

## Task 2: fila e integração BDA

Arquivos: stagingCopier.h/.cpp, bufferCache.h/.cpp, ShaderCoherenceCopyTests.inc, CMakeLists.
Interfaces: AcquireProtection(); EnqueueProtection(ProtectBatch) -> uint64_t.
FinishBdaBatchedUpload/UploadCopies recebem copier/ticket se a proteção está pendente.

- [x] Fixture nativo: worker preso, PrepareBda retorna antes da proteção, escrita CPU
  nesse intervalo chega à primeira cópia; RED contra o caminho síncrono atual.
- [x] Destacar somente passes sem hot ranges, modo 1/read, soma >= limiar F1.
  Runs pequenos desses passes também usam worker, sem espera antecipada.
- [x] Apply antes de Run(copy); completed somente depois; pool limitado.
- [x] Fallback por alias/reserva aguarda prefixo antes de memcpy; verify síncrono.
- [x] Testar 1/read/verify/read-verify, recorder 0/1, aliases, live disable,
  submit pendente, rechecagem de produtor e unmap com os fixtures reais F1.

## Task 3: diagnósticos, revisão e entrega

Arquivos: profiler.h/.cpp e documentação de arquitetura/diagnóstico F3.

- [x] CoherenceProtectJobs/Spans/Reuses/Fallbacks; tempos de proteção e espera.
- [x] Build Release do emulador e suite focada PageManager/tracker/F1/F3.
- [x] Revisão separada desde 07b069d, resolver falhas materiais.
- [x] Documentar resultado/comando opt-in e commit; FPS só com A/B nativo.

## Registro

Base: 07b069d. Preflight: Task 1 produz ownership de batch consumido na Task 2;
Task 2 produz tickets medidos na Task 3. Tipos e ordem FIFO coerentes.
F5 é a etapa seguinte com os contratos da seção 9; esta F3 não implementa settle.

Task 1: testes completos passaram; batch aplicada em outra thread, latest counts,
NoAccess, cancelamento, moves e nested fallback. Uma falha de compilação por macro
NO_COPY também proibir moves foi corrigida usando delete explícito só nas cópias.
Task 2 RED observado: coherence_protect_1_rec0 abortou exatamente em
pending protection returned: PrepareBda synchronously applied protection while its worker was held.
Artefato f3-native-red.log. Prosseguir com prefixo FIFO.

Task 2: prefixo Protect na mesma fila do StagingCopier, Apply antes das cópias
e do completed; um ticket por passe, com storage reutilizável (32 batches,
até 4.096 spans por batch retida). O shutdown drena mesmo com worker retido
pelo fixture. Verificação: proteção assíncrona, escrita no intervalo anterior
à cópia, aliases, live disable, submit pendente, reuso, unmap e destruição
com prefixo pendente; modos verify continuam síncronos.

Revisão: P1 reproduzida. Uma batch transferida já tinha count=1 com a página
RW; um segundo watcher síncrono passava de 1 para 2 sem aplicar RO. O teste
page_manager_protect_readiness falhou antes da correção (f3-readiness-red.log).
Agora UpdateCountsLocked também aplica a permissão ainda pendente para
consumidores síncronos. Coletores deferred mantêm sua batch. Seis casos
(linear/máscara × Off/On/Verify) passaram depois (f3-readiness-green.log).
A observação inicial de certificado DrawPrep foi retirada pelo revisor:
faltava prova de regressão específica da F3 nesse caminho.

Task 3: build Release aprovado, emulador e três targets de testes. Rodada final:
24/24 testes, zero falhas, 7,33 s (f3-final-tests.log). Inclui PageManager
completo, readiness, tracker, F1/F2/F3 e admissão BDA de novos buffers.
Fallback inline é condicionado somente ao ticket de proteção, não ao maior
ticket posterior de cópia; o fixture valida seus bytes, sem uma medição
separada da latência desse ramo. A/B da F3 no jogo permanece pendente.
Entrega documentada em docs/F3-COERENCIA-2026-10-08.md.
