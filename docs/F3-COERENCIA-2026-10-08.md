# F3 — proteção de uploads no worker

Data: 2026-10-08. Base: 07b069d. Worktree:
`/home/jonathanbraga/kyty-coherence`, branch `cpu-coherence-service`.

## Entrega

`KYTY_COHERENCE_PROTECT=1` transfere a aplicação das batches de write-watch
do Thread_Gpu para o StagingCopier já usado pela F1/F2. Padrão desligado.
Um prefixo de proteção por passe entra antes dos seus jobs de cópia.
O completed e a dependência de submit incluem esse prefixo.

Admissão: batching BDA ativo, modo de cópia `1` ou `read`, nenhum hot
snapshot no passe, bytes somados pelo menos no limiar F1 (16 KiB padrão),
escopo não aninhado. Modos verify/read-verify e read-watch/NoAccess
continuam síncronos. Runs pequenos de um passe admitido usam o mesmo worker.

A batch guarda spans, reavalia counts sob os locks atuais e aplica as
permissões mais recentes. Movimentação e cancelamento não descartam
proteções. Um consumidor síncrono que chega antes do worker aplica a
proteção necessária antes de retornar, mesmo na transição de count 1→2.
Assim snapshots de textura não ficam sem write-watch nesse intervalo.

## Custo e dependências

Storage volta a um pool de até 32 batches, cada uma com no máximo 4.096
spans retidos; sem thread adicional. Guards físicos, fontes versionadas,
leases e pool de ranges da F1/F2 continuam em uso.

Sem alias canônico ou sem espaço no staging, a cópia inline aguarda só o
ticket do prefixo de proteção. O caminho normal enfileira a cópia e grava
a dependência de submit. O shutdown drena jobs pendentes antes de destruir
o worker; o PageManager e os mappings precisam viver até esse drain.

Contadores: `CoherenceProtectJobs`, `CoherenceProtectSpans`,
`CoherenceProtectReuses`, `CoherenceProtectFallbacks`.
Tempos: `CoherenceProtect` (worker) e `CoherenceProtectWait` (fallback).
Medir também `CoherenceCopyGuardWaits` e `CoherenceCopyGuard`.

## Verificação

Build Release do emulador aprovado. Rodada final: **24/24 testes passaram**,
zero falhas, 7,33 s. PageManager completo, tracker, F1/F2/F3 com recorder
0/1, verify, aliases, live disable, publicação, unmap, reuso da pool e
destruição do worker com prefixo pendente.

Revisão encontrou uma corrida no segundo watcher síncrono. A regressão
foi reproduzida antes da correção e passou depois em seis combinações:
faixa linear/máscara × modos Off/On/Verify. A correção compara a permissão
aplicada com a necessária sob os dois locks existentes.

Artefatos em `/home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1/validation/`:

- `f3-native-red.log`: fixture detecta caminho ainda síncrono.
- `f3-readiness-red.log` e `f3-readiness-green.log`: regressão do watcher.
- `f3-final-build.log` e `f3-final-tests.log`: validação final.

A/B da F3 no jogo ainda pendente. Os 3 FPS informados pelo usuário foram
medidos antes desta F3, após ampliar o memo de texturas. Não atribuir esse
resultado à F3. F5/settle adiado ainda não implementada.

## Testar Yōtei

Use o mesmo ponto e câmera parada; compare retirando/adicionando apenas
`KYTY_COHERENCE_PROTECT=1`, com caches aquecidos.

~~~bash
cd /home/jonathanbraga/KytyPS5-fork
KYTY_RUN_DIR="$PWD/_Build/coherence-f1/run-crash" bash tools/run-u59.sh \
  --game "/run/media/jonathanbraga/SSD/PS5/Ghost_of_Yotei_extraido/eboot.bin" \
  KYTY_COHERENCE_COPY=read \
  KYTY_COHERENCE_PROTECT=1 \
  KYTY_SRT_VARIANT_READS=1 \
  KYTY_BDA_WRITES=candidates \
  KYTY_BINDLESS=1 \
  KYTY_TEXTURE_BINDING_MEMO_SLOTS=65536 \
  2>&1 | tee _Build/coherence-f1/yotei-f3.log
~~~

O binário atualizado é `_Build/coherence-f1/kyty_emulator`; o link em
`run-crash/kyty_emulator` aponta para ele. A cópia de uploads exige fontes
CPU limpas e guards de publicação existentes; F3 não muda a propriedade
de páginas GPU nem libera shaders adicionais.
