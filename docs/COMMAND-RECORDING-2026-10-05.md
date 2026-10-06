# CommandRecorder, CommandScheduler e CommandStream — 2026-10-05

## Evidências e escopo

Análise do fluxo produtor → ring SPSC → replay → gravação Vulkan → submissão →
aposentadoria por timeline. Referências locais:

- `CLAUDE-DRAW-HOTPATH-2026-10-04.md`: captura histórica do Crash com CP próximo de
  60 ms/frame; `Ring::WaitConsumed` e `CommitHead` perto de 6% cada. O recorder
  aparecia ocupado em 46% do tempo; 70% dessa parcela era `WaitPublished`, portanto
  espera por trabalho, não custo de execução Vulkan.
- A mesma captura atribuía 78% das esperas do recorder ao caminho de occlusion.
  O código atual já usa o sink nesse caminho. Esse custo histórico não é um ganho
  atribuível às mudanças deste relatório.
- `CHANGES-U59.md`: separação entre recorder e sequencer; estimativas anteriores
  não constituem medição A/B de FPS destas alterações.

Revisados também: publicação em lote, back-pressure, espera/retomada do consumidor,
modo inline, direct windows, crescimento do pool, callbacks de prioridade,
aposentadoria de recursos e consulta espaçada de progresso. O código já contém
vários desses mecanismos; as alterações abaixo removem trabalho redundante
identificado no caminho atual.

## Alterações

| Componente | Antes | Depois | Onde ajuda |
|---|---|---|---|
| CommandStream / Replay | `check(VerifyHash::...)` calculava o hash antes de descobrir que o pacote não tinha verificação | As 52 chamadas de hash ficam condicionadas à presença de `VerifyBlock` | CPU do replay com `KYTY_CP_RECORDER_VERIFY` desligado, sobretudo descritores e push constants |
| CommandRecorder / Drain | Um drain ocupado codificava e esperava um `DrainMarker` sem efeito nativo | Publica o trabalho pendente e espera a posição já escrita | Menos codificação, bytes, replay e possível reserva bloqueante no ring ao abrir janelas diretas |
| CommandScheduler / CommandPool | Consultava progresso antes de procurar buffers já aposentados na parte anterior ao cursor; consultava até com pool vazio | Procura em toda a rotação usando o tick conhecido antes de consultar; pool vazio cresce diretamente | Evita consultas de timeline e chamadas ao driver sem necessidade |

O drain elimina um pacote de 16 bytes no modo normal ou 32 bytes com verificação,
mais o bloco de sites quando habilitado. O opcode `DrainMarker` continua disponível
para testes e compatibilidade do stream. Drenagens ociosas continuam sem wake-up.

A busca do pool preserva a preferência de rotação e faz uma única atualização se
todos os slots estiverem ocupados. Slots ainda em voo continuam indisponíveis;
o crescimento mantém o drain necessário antes de `vkAllocateCommandBuffers`.

## Sincronização

`Ring::WaitConsumed` publica os bytes ainda pendentes e espera a posição liberada
pelo consumidor **após** os executores nativos e o fechamento do site. A espera é
do trabalho de host; não substitui uma espera pela conclusão da GPU. O produtor
não emite novos pacotes durante essa espera. Não foram alteradas as ordens de
memória nem o protocolo de wake-up.

O pool usa `KnownGpuTick()`, que considera a conclusão da GPU e o retorno da
submissão no host. Isso impede reutilização precoce quando a GPU termina antes
de `vkQueueSubmit` retornar.

As regras verificadas com Context7 e a documentação Khronos exigem sincronização
externa do command pool, inclusive durante gravação em buffers do mesmo pool,
e impedem modificar buffers pendentes. Esses requisitos continuam preservados.
Fontes: [Command Buffers / Command Pools](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)
e [Vulkan Guide — Threading](https://docs.vulkan.org/guide/latest/threading.html).

## Validação

Os testes novos falharam antes das correções: hash de replay desnecessário,
pacote extra nas duas variantes de drain e consultas desnecessárias do pool.

- CTest: `cp_recorder`, `command_pool_reuse` e `cp_sequencer` — 3/3 aprovados.
- Recorder: round-trip de todos os opcodes com verificação ligada/desligada,
  corrupção, sites, wrap do ring, back-pressure e produtor/consumidor em threads.
- Teste de drain: retém o consumidor antes da execução e confirma que o produtor
  não pode retornar; após a execução, confirma posição consumida, ausência de
  mismatch e nenhuma alteração na contagem ou tamanho dos pacotes.
- Teste de hashes: zero hashes no encode e replay sem verificação; com verificação,
  mantém hashes de argumentos, digest e checagens.
- Pool: rotação, parte anterior ao cursor, pool vazio, progresso e slots em voo.
- Compiladas as unidades de produção `commandRecorder.cpp`, `commandScheduler.cpp`
  e `commandStream.cpp` com Clang 18 no WSL, além dos executáveis de CPU.
- ASan + UBSan: recorder/stream e testes do pool aprovados, sem diagnósticos.
  A instrumentação cobre as unidades alteradas do stream e os testes; as bibliotecas
  auxiliares já compiladas foram ligadas sem instrumentação.

## Benchmark A/B de CPU

Release, Clang 18, WSL Ubuntu 24.04; ambos os executáveis no mesmo CPU lógico 0.
Uma rodada de aquecimento por executável e cinco amostras alternadas por versão.
Cada amostra contém 12 × 10.000 draws, seis pacotes por draw: push constants de
64 bytes, 20 descritores de buffer, pipeline, viewport, scissor e draw indexed.
Executor que conta chamadas sem calcular hashes; verificação e instrumentação
de teste desligadas; zero esperas por espaço no ring. As 720.000 chamadas foram
preservadas em cada amostra.

| Mediana | Antes | Depois | Variação |
|---|---:|---:|---:|
| Replay | 670,725 ns/draw | 230,128 ns/draw | −65,7% |
| Encode | 487,655 ns/draw | 478,617 ns/draw | −1,9%, dentro da variação observada |

Replay bruto antes: 1002,625; 646,623; 688,193; 670,725; 617,124 ns/draw.
Depois: 230,128; 209,129; 250,399; 344,671; 192,389 ns/draw.
Uma execução exploratória sem afinidade também mostrou redução de replay
(60,7%), mas houve variação no encode; ela não fundamenta a mediana acima.

Isso mede somente o stream em CPU com executor sem driver. O compilador pode
eliminar leitura de argumentos não usados por esse executor, e o resultado não
representa o custo completo de gravação Vulkan. Não é medição de FPS, frame time,
latência entre threads ou ganho no jogo. Não há medição temporal A/B do drain ou
das consultas do pool; nesses casos os testes demonstram o trabalho eliminado.

Reprodução: construir `cp_recorder_benchmark` e executar
`cp_recorder_benchmark --bench-replay-only`, comparando com um binário compilado
antes da alteração sob as mesmas flags. O contador de hashes é definido apenas
no alvo `cp_recorder_tests`, sem custo no benchmark ou nas unidades de produção.

## Limites e próximo perfil

O impacto visível esperado é menor uso de CPU do recorder e menor overhead em
drenagens e aquisição de buffers. FPS depende de esses caminhos estarem no
caminho crítico. O ganho precisa ser medido em hardware Vulkan compatível;
esta validação não equivale a um build completo nem a testes de GPU.

Comparar a mesma cena, cache e flags, medindo frame time p50/p95/p99, tempo ocupado
do recorder, `CpRecorderDrain`, `CpRecorderPackets`, ring waits e consultas de
timeline. Não converter porcentagens do perfil histórico em ganhos atuais de
FPS. Esperas por dados, conclusão da GPU e aposentadoria de recursos ainda podem
dominar o fluxo e precisam de medição no jogo antes de novas alterações.
