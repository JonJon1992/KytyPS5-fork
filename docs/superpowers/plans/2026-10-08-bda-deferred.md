# F5: settle BDA adiado

> Execução nativa: superpowers:executing-plans. Usuário autorizou implementar F5
> sobre F3 57f0d97 no worktree cpu-coherence-service; seguir até validação e commit.

**Goal:** remover a espera imediatamente após dispatches BDA sem antecipar dados
ou conclusões observáveis pelo guest.
**Architecture:** reservar domínio antes do writer, registrar produtores por
tick/geração, compactar para slots retidos, coletar no runner e aplicar no
Thread_Gpu. Publicações capturam o prefixo de tickets; leitores CP suspendem
antes de consumir páginas pendentes.
**Tech Stack:** C++ e wrappers Vulkan/recorder existentes.
**Spec:** docs/ARQUITETURA-ESCRITAS-RUNTIME-2026-10-08.md, seção 9.

## Contratos

- F1/F2/F3/F4 e código existente de loads permanecem ativos.
- Opt-in KYTY_BDA_WRITES=deferred, sem alterar defaults.
- Raw DWORD stores compute suportados pelo emitter atual; outros tipos continuam
  com as rejeições atuais. Hashes com prova F4 usam candidatos sem settle.
- Domínio conservador: entradas BDA registradas que o shader pode alcançar.
  Fazer uploads/preservação/proteção antes do writer, após a última preparação.
- Nenhum upload nem preservação posterior ao writer. A aplicação fecha a
  pendência depois de conferir o bitmap contra o domínio retido.
- A/B sobrepostos ficam separados. Epoch desliga certificados, fills e verdicts
  enquanto houver unknown writes; consultas exatas verificam pendências por faixa.
- Collector não aplica cache metadata nem espera o Thread_Gpu. Publicações só
  executam depois de Applied. Callbacks normais também retêm recursos até Applied.
- CPU espera só produtores da página; CP suspende a operação e permite outras
  filas. Nenhum memcpy do CP pode usar um fault como espera do próprio thread.
- Map/unmap/shutdown concluem referências antes de liberar gerações. Aliases
  físicos sem proteção anterior comprovada recusam admissão.
- Ring de 8 slots, reutilizado somente após CPU Applied. Overflow usa domínio
  retido inteiro; dropped/out-of-domain impede conclusão bem-sucedida.

## Revisão

1. Runner preso em label enquanto há coleta pronta: coleta deve avançar.
2. Tick final diferente do tick de preparação: retag antes de emitir.
3. A aplicado depois de um writer conhecido B: não regredir ownership/tick.
4. CPU readback/unmap encerrando tracker enquanto B ainda está pendente.
5. Alias novo ou existente permitindo raw access a backing ainda GPU-owned.

## Task 1: registro e epoch

Arquivos: novo graphics/host_gpu/bdaWriteLedger.h; tests/BdaWriteLedgerTests.cpp;
CMakeLists.txt.
Interfaces: Open(tick, mapping_generation, domain) -> ticket; PendingForRange;
Apply(ticket); AppliedPrefix; HasPending; UnknownWriteEpoch.
Owner escreve ledger; readers externos consultam somente atomics publicados.

- [x] RED: A/B mesma página, página independente, prefixo fora de ordem,
  geração nova, storage retido e limite de slots.
- [x] Implementar ledger com domínios ordenados e storage reutilizável.
- [x] GREEN: target host bda_write_ledger_tests.

## Task 2: coleta e publicação

Arquivos: cache/faultManager.*, commandScheduler.*, teste nativo dedicado.
Interfaces: RecordBdaWrites(slot) -> tick; ParseBdaWrites(slot) -> resultado;
ReleaseBdaWrites(slot); DeferCollectionOperation; gate por AppliedPrefix;
service hook para waits do proprietário.

- [x] RED nativo: native complete não libera publication antes de Applied.
- [x] Separar gravação/parse da coleta síncrona sem alterar barreiras existentes.
- [x] Fila de coleta independente dos labels; slots retidos até Applied.
- [x] GREEN: recorder 0/1, labels bloqueados, coleta posterior avança, teardown.

## Task 3: domínio, guards e integração

Arquivos: bufferCache.*, renderCompute.cpp, CodegenOptions.*, kernel/memory.*;
drawPrep/readSet/clean verdict/fills conforme os pontos proprietários.
Interfaces: PrepareDeferredBdaWrite -> ticket; QueueDeferredBdaWrite;
ServiceDeferredBdaWrites; PendingBdaWriteTick; WaitBdaWritesForRange.

- [x] Preparar domínio, preservar imagens e marcar ownership antes da execução.
- [x] Guardar geração/domínio; GC/retirement e mapping não liberam referências.
- [x] Kernel predicado sem cache recusa páginas pendentes; epoch invalida
  certificados e desliga atalhos de fills/clean verdict enquanto aberta.
- [x] Aplicação apenas confere resultado e fecha ticket; overflow conservador.
- [x] GREEN nativo: CPU raw reads antes/depois, A/B, writer conhecido posterior,
  overflow/dropped do coletor, alias sem cobertura, unmap e shutdown. O gate
  fatal dropped/out-of-domain foi revisado no owner.

## Task 4: CP e fronteiras observáveis

Arquivos: graphicsRun.*, commandProcessor.h, sync.cpp, scheduler conforme owners.
Interfaces: readiness por faixa antes do op; publicação captura prefixo;
serviço interno permanece disponível durante drain.

- [x] Suspender leitores CP antes dos efeitos e raw reads; testar CE/DE e sequencer.
- [x] Gate obrigatório para labels/EOP, interrupção sem label, GDS e flips.
- [x] Fronteira nativa em evento intermediário para não capturar writers futuros.
- [x] GREEN nativo: queue independente avança durante wait, label A não aguarda
  writer B independente, shutdown continua aplicando resultados.

## Task 5: entrega

- [x] Build Release do emulador e suite focada ledger/scheduler/memória/F1-F5.
  Resultado: 53/54 passaram; falha FMASK reproduzida na base e documentada.
- [x] Revisão fresca do contrato alterado; achados materiais corrigidos.
- [x] Documento F5 com limites, métricas, comandos e artefatos de validação,
  pronto para o commit de entrega no worktree cpu-coherence-service.

Regra de medição: ganho de FPS só com A/B do mesmo trabalho na RX 9070 XT.
Esse A/B no jogo permanece pendente; não é um resultado das fixtures.

## Registro de execução

- Base de rollback: F3 57f0d97; escopo integral no worktree cpu-coherence-service.
- Ledger host: RED com 13 falhas, implementação GREEN com 0 falhas.
- Scheduler: RED publicou antes de Applied; core GREEN em recorder 0/1 e
  command_scheduler_timeline. Nativo core: A/B, raw CPU e aliases passaram.
- CP COND_EXEC: RED reproduziu bloqueio na própria página pendente. Preflight
  de reads e suspensão/retry aplicados antes dos efeitos.
- Revisão fresca: corrigidos retry do head próprio do DrawPrep, labels diante
  de writers posteriores, tabelas dinâmicas de vértices, índices do fallback,
  partial replay de multi-draw e redução da history indireta. Leituras clean
  também recusam labels pendentes antes de consultar verdicts cacheados.
- Fixture expandida aguarda o handoff real do label ao owner; seu marcador
  sintético anterior não prova que esse handoff já executou.
- Validação final concluída fora do sandbox na RX 9070 XT: build Release
  aprovado e 53/54 testes passaram em 26,89 s. Logs: `f5-final-build.log` e
  `f5-final-tests.log`, em `_Build/coherence-f1/validation/` da árvore principal.
- Única falha: `resource_tracking`, assertion FMASK em `ir/Value.cpp:188`.
  Recompilar as 11 unidades do alvo e headers alterados a partir de F3 `57f0d97`
  reproduziu a mesma falha; archive comum reutilizado sem fontes modificadas.
  Evidência: `f5-resource-baseline.log`.
- Revisão fresca encerrada sem achados materiais restantes em probes,
  certificados, metadados com apenas label pendente e KnownFill por faixa.
- Domínios reutilizados; busca binária limita o trabalho de conferência às
  interseções; reset do download faz flush só dos contadores. Game A/B pendente.
