# F4 e F2 — 8 de outubro de 2026

Base: F1 9956c566 no worktree kyty-coherence, branch cpu-coherence-service.
A fase 0 do Claude (8288d755, 21b00d82, c5ffb21a) foi integrada como referência,
preservando as correções finais da F1. Pedido: seguir com F4 e F2.

## F4 — destinos finitos

KYTY_BDA_WRITES=candidates habilita só 86da5eb7b8257bb0 e d8959888aafd2552.
Use também KYTY_SRT_VARIANT_READS=1 para os V# de runtime. As opções de shader
são lidas no início; KYTY_COHERENCE_COPY pode mudar ao vivo.
Water Lighting enumera os cinco V# em tabela+32..96; Shadow Resolve lê o contador
em tabela+0x70 em cada dispatch. O limite quatro não é presumido.

Admissão: até 64 descritores e 256 MiB somados. Valores excessivos, tabela
GPU-modified, regiões parcialmente mapeadas, auto-modificação da tabela,
swizzle, tabela desalinhada, OOB_SELECT=2 e OOB_SELECT=3 com stride positivo
rejeitam o dispatch. Nada é truncado. Destinos recebem
o tratamento normal de bindings graváveis, incluindo preservação de imagens.

Após o último PrepareBda, salva-se a tabela GPU, envia-se o snapshot enumerado,
executa-se o dispatch e restaura-se a tabela original. Um único buffer de
dispositivo, alocado no primeiro uso, é reutilizado na mesma fila, sem
alocação por dispatch. A restauração preserva versões mais novas da CPU.
As leituras escalares de endereços desses shaders permanecem na GPU: contadores e ponteiros usam a
mesma tabela congelada, sem slots SRT de uma versão anterior.

O modo candidates omite bitmap e settle. candidates-verify mantém os dois e
falha em páginas fora da união, escritas descartadas ou overflow (comparação
por página). PCs e forma dos stores são conferidos no recompilador. Program
cache distingue fase 0, candidates e candidates-verify (layout da chave 5).

Falha de prova mantém o comportamento conservador de pular o dispatch.
Para a referência síncrona, retire candidates e mantenha os dois hashes em
KYTY_BDA_WRITES_SHADERS. Não há execução ilimitada em caso de prova incompleta.

Arquivos: bdaWriteCandidates.h, CodegenOptions, ResourceTracking, emitter de
memória, bufferCache, renderCompute, programDiskCache/pipelineCache e profiler.

## F2 — uploads de leitura

KYTY_COHERENCE_COPY=read inclui F1 e uploads de bindings de leitura do cache.
read-verify inclui comparação de fonte e readback nativo. Os modos 0, 1 e
verify mantêm o escopo anterior.

Admissão depois da proteção, fora dos locks do tracker. F1 e F2 compartilham
uma função, worker, dependência de submit, guards de aliases, lease de
publicação e pool de vetores. Uploads graváveis, snapshots hot, pequenas
cópias de stream e reservas temporárias usam o caminho existente. Limiar
live KYTY_COHERENCE_COPY_MIN_KB: 16 KiB. Com KYTY_UPLOAD_BATCH=0,
a mesma dependência de publicação usa uma cópia nativa com um par de barreiras
para todas as regiões; o modo de produção continua aproveitando o batching.

## Verificação concluída

Build Release do emulador e testes aprovado. 36 testes distintos passaram
durante a validação; a rodada final de 12 validou todos os modos F1/F2,
F2 sem batching, F4 com recorder 0/1 e program cache. Os dois erros iniciais
do fixture F4 (scheduler não iniciado) e o assert com batching desligado
foram corrigidos; as variantes correspondentes passaram na rodada final.

Revisão separada de código confirmou as correções de OOB_SELECT=3, tabela
desalinhada, coerência do count GPU e as barreiras do ramo sem batching.
Nenhuma falha concreta ficou aberta nessa revisão.

Os stores dos binários reais do program cache também foram conferidos:

| Shader | PC | Instrução |
| --- | --- | --- |
| 86da5eb7b8257bb0 | 0x530 | buffer_store_dword v0, v1, s[4:7], 0 idxen glc |
| d8959888aafd2552 | 0x1b4 | buffer_store_dword v0, v3, s[4:7], 0 idxen glc |

Artefatos em /home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1/:

- kyty_emulator: binário atualizado; run-crash/kyty_emulator aponta para ele.
- validation/f4-f2-build-release.log: build final aprovado.
- validation/f4-f2-ctest.log: primeira rodada e 33 passes iniciais.
- validation/f4-f2-ctest-release.log: 12 passes finais após as correções.
- validation/real-writers/: códigos extraídos usados para conferir a forma dos stores.
- run-crash/mode.py: aceita read e read-verify, sem iniciar o jogo.

## Medição no jogo ainda pendente

Testes focados: limites/OOB/cache keys, stores BDA nativos com e sem bitmap,
proteção/readback dos destinos, save/restore da tabela alterada pela CPU,
read/read-verify, aliases, desativação live, submit, pool e unmap.

A/B no jogo: mesma cena/câmera e caches; fase 0 → candidates-verify para
correção → candidates para tempo; F1=1 → F2=read.
Medir BdaSettle, BdaCandidateDispatches/Rejects/Misses, tempo/fps,
CoherenceCopyJobs/Bytes/RangeReuses, CoherenceCopyGuardWaits, CoherenceCopyGuard e CoherenceCopyGuardLookup e
zero mismatches. Ganho de fps ainda não medido nesta implementação.

Contratos Vulkan seguem os wrappers existentes e documentação Khronos:
https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/memory.adoc
https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples
