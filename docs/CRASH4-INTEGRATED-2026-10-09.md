# Crash 4 — build integrado e A/B em jogo, 2026-10-09

Build integrado com as alterações locais, incluindo o patch de backing-map
cache/binding stats do Claude. Executável instalado em `linux-clang/install`
e `crash-run`, com backups e manifesto em `_Build/integrated-crash-20261009/`.
SHA-256: `524432cdccd54eeaad1446c59362c1973069b52fd9f570c16daf74a668ee7988`.

Build e smoke passaram; 24 CTests focados passaram e sete casos GPU Wolverine/RT
passaram com zero erros de validação sync. A suíte CFG completa ainda falha em
`guarded LDS loop did not exercise deferred Phi exit-label patching`.

## Método

Sessão `crash-run/runs/20261009-233529`, PID 170249, perfil Crash em 1080p.
O usuário confirmou a cena e relatou imagem normal durante a coleta inicial.
Logging de bindings, HangTrace e controle live permaneceram iguais nos dois
braços. As opções experimentais que exigem startup não foram ativadas.

Janelas de aproximadamente cinco segundos, alternância A/B, com um segundo de
acomodação. FPS medido por flips do CP entre confirmações live; não é FPS de
apresentação independente. CPU por thread veio dos ticks de `/proc`, divididos
pelos flips; isso não equivale ao tempo decorrido de execução de uma função.

A primeira tentativa teve compilação concorrente e foi descartada; a segunda
foi interrompida ao detectar outro build. Ambas foram preservadas em diretórios
separados. A coleta final verificou ausência de processos de compilação ativos
no começo/fim das janelas. Isso não garante ausência de toda atividade externa.

## Resultado final

Valores abaixo são medianas entre janelas, não percentis de frames individuais.

| Comparação | Referência | Candidata | Decisão |
|---|---:|---:|---|
| Cache de backing: 4 vs 16 entradas (12 pares) | 25,81 FPS | 25,72 FPS | Manter 4 |
| DrawRun quiet: off vs on (6 pares) | 26,10 FPS | 26,06 FPS | Manter off |
| CPU Thread_Gpu, cache 4 vs 16 | 28,75 ms/frame | 28,89 ms/frame | Sem redução demonstrada |
| CPU Thread_Gpu, quiet off vs on | 28,07 ms/frame | 27,99 ms/frame | Diferença pequena |

Cache: houve oscilação forte nas primeiras janelas (mínimo de 14,71 FPS no
braço 4). A mediana da variação pareada foi +0,01%; não se descartaram essas
janelas depois de ver os resultados. A média pareada ficou em +3,40%, dominada
por variação, com intervalo bootstrap exploratório de -2,70% a +12,12%.

DrawRun: média pareada +0,13%, intervalo exploratório de -0,80% a +1,01%.
Esses intervalos usam reamostragem de pares de janelas, não frames; a ordem fixa
A/B e a possível autocorrelação limitam a interpretação estatística. Nenhum
resultado justifica promover uma das opções.

As linhas `Bindings 10s` atravessam mais de uma janela de cinco segundos;
portanto, não foram atribuídas integralmente a um braço para alegar mudança
de hit rate. O FPS destas rodadas não foi comparado à captura antiga como
prova de ganho do conjunto de patches.

## Estado final e pendências

Cache restaurado para 4 entradas (seq 304); quiet restaurado para off (seq 341),
com confirmação live. O jogo permaneceu aberto ao final. Nenhum preset foi
promovido com base nesses resultados.

Permanecem pendentes A/B das opções de startup (read-only por binding,
uniformidade, indirect count, coalescimento de barriers), análise de shaders
representativos, percentis por frame e validação prolongada. Nesta cena, os
dados de CPU da RenderThread e do Thread_Gpu continuam relevantes para o próximo
perfil; não demonstram por si só a função causadora do gargalo.

Artefatos: `bindings-ab.json`, `bindings-ab.summary.json`, `bindings-ab.log`,
`quiet-ab.json`, `quiet-ab.summary.json`, `quiet-ab.log`, scripts, logs por
janela e `artifacts.json` em `_Build/integrated-crash-20261009/`.
