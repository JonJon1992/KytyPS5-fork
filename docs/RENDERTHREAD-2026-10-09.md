# Crash 4: RenderThread e espera dos workers

## Evidência e limite da medição

Na sessão 61948 (executável `9b5a07a-dirty`), o perf coletou 2.668 amostras
do RenderThread. Dessas, 1.826 (68,44%) estavam na rotina convidada
`0x9014e88b0–0x9014e8a30`, com leituras sincronizadas e operações atômicas de
fila. Isso indica sincronização/possível contenção; não prova que todo o
tempo seja espera nem identifica o produtor atrasado.

Um teste A/B/A de afinidade deu 28,69 / 25,19 / 28,38 FPS: fixar o
RenderThread no núcleo físico 2 piorou o resultado. A afinidade foi
restaurada. As janelas tinham 20 segundos, sem alterar qualidade gráfica.

Uma segunda coleta, de todas as threads, mostrou espera ativa nos workers
de draws, em `CpSeq::Sequencer::WaitSlow` e no gravador de comandos. Por
isso, a primeira mudança trata o tempo de espera dos workers, sem alterar
o código convidado nem sua sincronização.

## Controle de espera ao vivo

`KYTY_DRAW_PREP_SPIN_US` e `KYTY_DRAW_PREP_COLD_SPIN_US` agora aceitam
alterações por `KYTY_LIVE_FILE`. Os padrões permanecem 200 e 50 microssegundos.
O orçamento é consultado a cada 256 tentativas ociosas, junto à consulta de
relógio que já existia. Um worker já ocioso também percebe a redução.

O ajuste controla somente quando estacionar a thread. Publicação, aquisição
de tarefas, ordem dos draws, notificações e encerramento mantêm o mesmo
protocolo. Threads já estacionadas continuam acordando pela publicação de
trabalho, não pela mudança de orçamento.

## Validação

- Build de `draw_prep_tests` e `kyty_emulator` concluído.
- `draw_prep_tests` completo passou: estresse da fila, tokens de acordar,
  execução única e ordenada, além do novo teste de redução do orçamento
  durante a ociosidade de workers hot/cold, retomada e encerramento.
- Cópias do executável em `_Build/linux-clang/install` e `_Build/crash-run`
  atualizadas; backups em `_Build/renderthread-20261009`.

A escolha de um orçamento menor depende do A/B no jogo. Esta mudança,
isoladamente, habilita a medição; não representa um ganho de FPS confirmado.
Artefatos e scripts locais: `_Build/renderthread-20261009/`.

## A/B parcial no executável atualizado

Sessão 88903, build `321aa65-dirty`, opção read-only por binding desligada.
O jogo ficou na cena escolhida pelo usuário. O controle ao vivo registrou os
frames e o tempo exatos nas bordas de cada janela de aproximadamente 20 s,
depois de 3 s de acomodação. CPU por thread foi obtida de `/proc`, sem perf
ativo; utilização de GPU foi amostrada a cada 0,5 s no sysfs.

| Métrica | Padrão 200/50 µs | Ajuste 50/0 µs |
| --- | ---: | ---: |
| FPS | 14,70 | 14,82 |
| Tempo médio entre flips (ms) | 68,02 | 67,49 |
| CPU DrawPrep, soma dos workers (ms/frame) | 78,49 | 57,71 |
| CPU Thread_Gpu (ms/frame) | 53,66 | 53,46 |
| CPU RenderThread (ms/frame) | 66,04 | 65,78 |
| CPU do processo, núcleos equivalentes | 4,95 | 4,64 |
| Utilização média da GPU (%) | 45,48 | 45,70 |

O ajuste reduziu aproximadamente 26,5% do tempo de CPU dos workers DrawPrep,
mas a diferença de FPS de 0,8% não demonstra ganho de performance: a sessão
terminou durante o retorno ao padrão, impedindo completar A/B/A. Os
relatórios de 10 s mostraram zero fallback serial nos dois casos; a preparação
feita pelo próprio consumidor cresceu ao reduzir spin. A nova sessão foi
aberta com RenderDoc e sem controle ao vivo e não entra nessa comparação.

Os orçamentos foram restaurados para 200/50 µs. Nenhum ajuste de espera foi
colocado no perfil padrão. 1% low, p95/p99, GPU frame time, readbacks/barriers
por frame e comparação visual formal não foram coletados nesta sessão.
Artefatos preservados: `live-sweep-partial-88903.json`,
`live-sweep-partial-88903.log` e `live-window-A1.log` /
`live-window-spin50_0.log`, no diretório acima.
