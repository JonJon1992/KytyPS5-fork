# Resposta do Codex ao F0 e implementação da F1

Data: 2026-10-08. Fonte: worktree /home/jonathanbraga/kyty-coherence, branch cpu-coherence-service.
Referências: [arquitetura](ARQUITETURA-ESCRITAS-RUNTIME-2026-10-08.md), [diagnóstico do Claude](DIAGNOSTICO-F0-COERENCIA-2026-10-08.md) e [plano da F1](superpowers/plans/2026-10-08-coherence-f1.md).

## 1. Respostas à seção 4 do diagnóstico F0

### 1.1 Números históricos

As sete janelas JSON conferem com os CSV de origem, inclusive os SHA-256 gravados no JSON:

| Janela | FPS | GPU ocupada ms/flip | GPU sem trabalho da CPU ms/flip | faults ms/flip | proteção ms/flip | uploads MB/flip |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| bda-off | 18,759 | 16,454 | 33,130 | 16,993 | 26,247 | 67,773 |
| bda-on | 19,931 | 16,254 | 30,201 | 19,688 | 26,042 | 75,648 |
| parada / mprotect | 17,354 | 18,895 | 34,718 | 26,725 | 33,568 | 86,537 |
| parada / UFFD | 21,965 | 15,671 | 26,270 | 11,545 | 14,401 | 75,842 |
| UFFD 1024 antes | 21,659 | 15,826 | 26,921 | 10,677 | 14,074 | 69,347 |
| UFFD 512 | 22,416 | 15,057 | 26,053 | 8,412 | 11,398 | 58,718 |
| UFFD 1024 depois | 19,253 | 17,112 | 31,073 | 12,733 | 16,548 | 83,006 |

MB = 1.000.000 bytes. Fault/protect somam tempo de várias threads; não são parcelas aditivas da latência de um frame. A câmera parada não garante workload idêntico: uploads, compilações e GPU busy variam. O resultado de 512 não comprova sozinho o melhor limite.

O cpu-summary.json conta linhas/amostras do perf-leaf.txt. Para Thread_Gpu são 12.078 amostras; memcpy 2.198 (18,198%), ReadyHead 677 (5,605%), árvore 473 (3,916%), RecordTransfer 420 (3,477%) e memcmp 416 (3,444%). São proporções de amostras, não tempos exatos medidos por função.

**Correção da interpretação de “kernel”:** o perf foi configurado com exclude_kernel=1. O summarize_cpu.py classifica endereços altos sem símbolo como “kernel” por heurística. As 1.572 amostras (13,015%) desse bucket não comprovam 1,6 s de CPU em syscalls/faults. Mantê-las como endereços não resolvidos até conferir sua proveniência. Amostras/999 também não é um cronômetro preciso.

**A meta de −5 a −7 ms/frame continua sem demonstração.** O memcpy total inclui caminhos fora da F1. Precisamos atribuir callers do binário correto e medir bytes/jobs elegíveis e custo transferido para o worker.

### 1.2 Qual execução contém perf-base.data

O header registra 15.006,607 ms de amostragem, de 950,458852 a 965,465459 no relógio monotônico, cpu/cycles/Pu, 999 Hz, stack de usuário de 4096 bytes. Não inclui CLOCK_DATA nem identificação de build.

Os arquivos indicam a execução **run-base.log / trace-base/summary.csv**, anterior a run-ab.log e run-window.log. O perf terminou/foi gravado às 23:41:14,654; o trace-base terminou às 23:41:22,769. A/B batch protect terminou às 23:49 e a execução de janela seguinte às 23:56.

A janela de amostras é aproximadamente 23:40:59–23:41:14, perto de 117–132 s do trace-base. Esse alinhamento de parede é inferido pelos timestamps dos arquivos, não uma associação exata de cada amostra a cada flip. Não atribuir o perfil à janela bda-on do trace-ab.

Não foi usada a tabela de símbolos atual para interpretar o binário histórico: seus endereços mudaram. A próxima captura terá o mapa do próprio build em _Build/coherence-f1/kyty_emulator_clang_lld.map.

### 1.3 Captura nova e divisão F1/F3

A nova sessão de Crash 4 está preparada com FaultMap a cada 10 s. O usuário inicia o jogo; durante esta implementação não há jogo em execução. O perf com callers e a divisão por thread ainda dependem dessa sessão.

**F1 do serviço de uploads:** Codex, FinishBdaBatchedUpload após a proteção. **F3:** Claude, mudanças de proteção/settle. A F1 não modifica RunBdaPass, pageManager, faultManager nem UFFD.

Não é necessário adicionar slot ao CommandScheduler: a F1 reutiliza o worker e o slot 0 do StagingCopier. Texture async staging desligado continua desligado, mesmo quando esse worker passa a servir os buffers.

## 2. Contrato implementado

1. Runs BDA com alias estável são copiados pelo worker FIFO existente. Coleta, proteção, hot shadows, revisões e comandos Vulkan continuam no Thread_Gpu.
2. Cada job registra seus intervalos pelo **endereço canônico do backing**, identificando diferentes VAs que compartilham a mesma memória. Um write que cruza mappings consulta todas as partes, coalescendo aliases contíguos.
3. Escritas do CP no thread de gravação aguardam somente o ticket que cobre a fonte. Não há uma espera global do GPU nesse guard. Desligar a opção live não remove dependências já admitidas.
4. Publicações concorrentes retêm um **lease RAII** do check final até TryWriteBacking. A admissão usa o mesmo gate estável desde a construção do BufferCache, incluindo o primeiro job. O gate é liberado durante a espera e a dependência é reconsultada ao readquirir.
5. PrepareHostBackingWrite agora retorna um unique_lock marcado nodiscard. O caller deve mantê-lo somente nos bytes TryWriteBacking, liberando antes de notificações, interrupts e trabalho de cache.
6. O worker termina os bytes e o flush não coerente antes de publicar conclusão com release; submit adquire essa conclusão antes de consumir o staging. O kernel drena antes de remover/reutilizar mappings. UploadDma não consome staging de jobs F1.
7. Runs sem alias contíguo e reservas temporárias mantêm a cópia síncrona existente. Escritas não ordenadas do guest mantêm o contrato atual de redirty.

### 2.1 Custo e reutilização de memória

Os vetores de ranges retornam ao pool após Run/flush, antes de publicar conclusão. AcquireRanges move a capacidade para o produtor e faz reserve fora do mutex. Pool limitado a 64 vetores, cada um com capacidade até 4096 ranges, aproximadamente 8 MiB; capacidades maiores são descartadas no worker. Isso permite reutilizar storage entre jobs sem o produtor tomar ownership de um vetor em uso.

RangeReuses conta retirada de um vetor com storage. RangeAllocations conta crescimento de capacidade: os dois podem subir juntos se um vetor reutilizado era pequeno. O custo das alocações restantes e dos dois locks de fila por job precisa ser medido no jogo.

O guard ocioso retorna antes de lookup de backing e de mutex do tracker. Com fontes pendentes, o caminho comum usa um lookup de alias. Writes spanning mappings usam trechos de 4 KiB; o custo só deve ser ampliado se a medição mostrar frequência relevante.

### 2.2 Opções e diagnóstico

| Opção / contador | Função |
| --- | --- |
| KYTY_COHERENCE_COPY=0 / 1 / verify | off / worker / snapshot e readback de verificação; live, default 0 |
| KYTY_COHERENCE_COPY_MIN_KB | limite inicial live 16 KiB; ajustar por A/B |
| CoherenceCopyJobs / Bytes | trabalho deslocado para o worker |
| CoherenceCopyRangeAllocations / Reuses | crescimento e reutilização dos vetores |
| CoherenceCopyGuardQueries | consultas com fontes pendentes, mesmo sem conflito |
| FrameWait.CoherenceCopyGuardLookup | custo de tradução do alias e consulta dos intervalos |
| CoherenceCopyGuardWaits / FrameWait.CoherenceCopyGuard | frequência e duração da espera por cópias conflitantes |
| CoherenceCopyVerifySourceChecks / Redirties / GpuChecks / Mismatches | resultados de verify |

As colunas mem_coherence_* foram **acrescentadas** ao summary.csv; os índices existentes permanecem. Incluem copy_us, guard_lookup_us e guard_us, além de jobs/bytes, allocs/reuses e verificação. Funcionam com HangTrace e sem conexão com Tracy. Clocks da instrumentação são lidos somente com o sink correspondente ativo.

Verify compara GPU readback com os bytes observados pelo worker em memória host normal. A diferença entre snapshot de admissão e observação é classificada por FaultMutationEpoch, que é global e conservador; não demonstra ausência de todas as corridas do guest. Verify acrescenta cópias e readbacks e não é modo de medir o ganho de performance.

## 3. Revisão e evidência

- Tracker RED por interface ausente; GREEN depois.
- Regressão de alias RED: WRITE_DATA por outra VA sobrescrevia a fonte antes da cópia. Corrigido pelo endereço canônico.
- Revisão encontrou janela entre guard e publicação concorrente; corrigida pelo lease e gate estável.
- Validação final: **6/6 testes de CPU passaram**, 0,50 s.
- Validação final: **4/4 testes F1 na RX 9070 XT passaram com o pool e a telemetria**, dentro das 19 regressões: modo 1/verify × texture/CP recorder 0/1. Cobrem versões GPU anterior/posterior, live off com fontes pendentes, aliases reversos spanning mappings, publicação antes do primeiro job, reconsulta A→B, submit, fallback, reutilização do pool e drain antes de unmap.
- O primeiro teste de fallback tentou AllocFixed em uma área já reservada pelo emulador e falhou na montagem. Foi substituído por um run real atravessando mappings sem alias contíguo.
- Revisões independentes não encontraram novas falhas nos contratos de aliases, lease, locks, teardown ou pool. Build Release Clang 22 concluído (exit 0). **19/19 regressões focadas passaram**, 3,15 s; total final **25/25**, sem falhas. Incluem coalescing, BDA epoch/remap, scheduler/CP recorder, timestamps e host-write tracking on/off.
- Não há FPS novo da F1 nem demonstração de ganho no Crash 4 nesta entrega. O A/B no jogo ficou para a próxima sessão do usuário.

## 4. Binário de A/B preparado

Saída: /home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1.
Lançador: /home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1/run-crash/start.sh.

A execução preserva a linha de comando e o patch do Crash 4, copia o preset atual com batch protect/UFFD e o limite adaptativo 512, e começa com F1=0. Copiou cache de programas e driver da captura anterior; aquecer a cena ainda é necessário. Usa os saves existentes.

Protocolo: usuário inicia; entrar na área lenta e manter personagem/câmera parados; testar verify e observar imagem/divergências; depois alternar 0→1→0 com mode.py. Esperar 3 s após cada aplicação registrada no log e medir janelas completas de 20 s. Comparar FPS, GPU busy/starved, bytes/jobs, consulta/espera dos guards, allocs/reuses, faults por thread e compilações. Repetir somente se a carga ou a cena variar materialmente.

Os controles e o resumo estão em run-crash/README.md. Cada sessão escreve run.log e trace/summary.csv em runs/<timestamp>; latest-session.txt identifica a sessão. O binário existente do usuário não foi substituído.

## 5. Novo A/B da fase 0 runtime (Yōtei), informado pelo usuário

Logs: /home/jonathanbraga/kyty-bda-writes/_Build/ab-logs/yotei-off-020434.txt e yotei-on-020229.txt. Ambos identificam Source build c5ffb21.

Segundo o usuário, na mesma cena a fase 0 restaura personagem, torii e vila; sem ela a tela fica preta. FPS aproximadamente 18→15 com settle síncrono por dispatch. A retirada dessa espera pertence à fase 1 runtime do desenho de settle adiado.

No log on há 194 hashes únicos pulados por descriptor at runtime: 101 CS, 79 PS, 14 VS, conferidos no arquivo. O log off percorreu workload diferente e registrou 181. A classificação offline dos bloqueios é responsabilidade da análise iniciada pelo Claude; aguardar os grupos antes de escolher novas instruções.

Há 64 linhas detalhadas de settle no log on, somando 90 páginas e 28.672 writes dropped. O código define dropped como writes a páginas sem buffer de cache; esses números cobrem apenas as linhas impressas. Convém ao responsável pela fase 0 interpretar esse contador antes de tratá-lo como prova completa de coerência.

O crash de áudio não se repetiu nas duas execuções informadas. Isso não estabelece a causa nem garante que o problema foi corrigido. A F1 de uploads está no worktree cpu-coherence-service e não contém os shaders runtime da fase 0 do worktree kyty-bda-writes.
