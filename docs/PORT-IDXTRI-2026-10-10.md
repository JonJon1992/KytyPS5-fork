# Triagem do fork IDXTRI/KytyPS5 — 2026-10-10

Pedido do usuário: analisar https://github.com/IDXTRI/KytyPS5. Base local: `guest-sync-release-mem`
@ `cd010298`. Refs buscadas: `refs/idxtri/{main,wolverine,wolverine-perf,wolverine-v1}` e, para
comparação, `refs/senaxx/{main,wolverine}`.

**Conclusão: nada urgente para portar.** Um item pequeno de input vale a pena, um medidor de
Thread_Gpu vale um A/B, e o resto já temos, é diagnóstico do Wolverine ou corrige código do
upstream que ainda não entrou aqui.

## O que há no fork

| Branch | Conteúdo | Situação |
|---|---|---|
| `main` (`b3e419ff`) | upstream de 2026-10-02 | já está no nosso histórico |
| `wolverine` (`758d87fc`, 80 commits) | cópia antiga (2026-10-02) do `Senaxx/KytyPS5:wolverine` | 79/80 assuntos ainda existem no Senaxx atual; triar lá, não aqui |
| `wolverine-perf` (`cac0d179`, 171 commits) | trabalho do DXTR sobre o PR #937 | **já analisado** em 2026-10-03 (`docs/DIVISAO-TRAVAMENTOS.md`, "Análise de `wolverine-perf`") |
| `wolverine-v1` (`fe1d8b1b`, tag de release) | `wolverine-perf` + merges do upstream (e6cb880, f53e5d2) + **58 commits novos** | objeto desta triagem |

Dos 58 commits novos, 5 são cherry-picks do upstream que já temos com outro hash (`f3dc5ee4`,
`b86cf0a8`, `64eea6b3`, `3353c1ff`, `71017416`). Ficam 53 commits, todos do DXTR, de 2026-10-02 e
2026-10-03, medidos em Windows/RTX 4070 Ti no Wolverine. Os números não valem para RX 9070 XT/RADV.

## Já temos (equivalente ou melhor)

| Commit | O que faz | Aqui |
|---|---|---|
| `f52046d7` | bindless: a chave cujo T# da heap mudou é reassentada; a imagem antiga é despinada (Senaxx `0563e10e`) | `bindless-cow` guarda o T# por chave e reresolve as que mudaram (`bindlessTable.h:81`, `descriptors.cpp` ~1543) |
| `b076dee9` | gather com LOD explícito e mip linear: aproxima em vez de pular o draw | `ResourceMaterialization.cpp:1448` já aproxima, com aviso |
| `dd9165f2` | `KYTY_BOOL_PREDICATION_WAIT`: tira o dreno da predicação bool (upstream `840b9f57`) | `KYTY_PREDICATION_NO_DRAIN` (`graphicsRun.cpp:2168`) |
| `9b1ad70e` | estatísticas de oclusão e um teto de cull, para medir quanto oclusão real ganharia | temos oclusão no host (`KYTY_OCCLUSION_{BATCH,GATE,PROXY_MODE,...}`) |
| `f7f1949a` | `KYTY_LABELS_AFTER_GPU` ligado por padrão | a nossa publicação de labels é outra (coerência F0–F5) |
| `fb54e221` | cache de pipeline do driver vale entre revisões | assinatura `KytyPC2` sem a revisão (`pipelineCache.cpp:129`) |
| `53a6a6c1` | `KYTY_FUNCTION_ARRAY_SHRINK` (Jetsku) | já portado; o próprio autor diz que no Wolverine piorou o stutter |

## Corrigem código do upstream que ainda não temos (levar no próximo merge do upstream)

- `316229f5` (do Senaxx `edc4532a`/`e6f33fdb`):
  - mesh fast-launch: o VGPR de instância não soma a instância inicial. Sem isso, a cena de título do Wolverine ficava escura.
  - capacidade de primitivas via `PRIM_AMP_FACTOR`.
  - finite images só para handles amostrados (gather derrubava o emulador no boot).
  - Hoje não temos `fast_launch` nem `TryMakeFiniteImage`.
- `cb87a709`: o laço do upstream (#883) que zera os argumentos de `GetSamplerResource` antes do SPIR-V também zerava a chave do sampler bindless, então todo material usava o registro 0 da heap.
  - O nosso `CompileProgram` não tem esse laço, e `BindlessSamplerSlot` (`spirvEmitterImage.cpp:1058`) lê `Arg(0)` direto.
  - Se esse laço entrar num merge, aplicar o fix junto.
- `8b3bf3e7` e `b9198835` (`KYTY_SKIP_BVH_DISPATCHES`): ajustes do merge deles. Não se aplicam.

## Candidatos

1. **`583f8385` — View/Back do controle = touchpad (7 linhas, risco baixo).**
   - Hoje `SDL_GAMEPAD_BUTTON_BACK` não mapeia para nada (`window.cpp:77`), então um controle Xbox não tem como apertar o touchpad.
   - O commit mapeia o botão e põe um toque no centro do touchpad enquanto ele está apertado.
   - Portar como está.
2. **`01d8d2ae` — contar as consultas `vkGetSemaphoreCounterValue` (A/B, sem mudar o padrão).**
   - O nosso `CommandScheduler::IsFree` (`commandScheduler.cpp:727`) faz o mesmo que o deles: consulta o driver sempre que o tick não está confirmado.
   - Os chamadores fazem polling por draw bindless, por leitura de label e por checagem de readback.
   - No RADV isso é um ioctl de syncobj.
   - Portar só `KYTY_TICK_STATS`, medir no Crash 4 e no Yotei, e só então pensar em `KYTY_TICK_POLL_US`. Ele atrasa a reciclagem; `Wait` continua exato.
3. **`c886342d` — `VirtualRanges` com `shared_mutex` (ganho provavelmente pequeno).**
   - O nosso `ClampRangeSize` já responde sem lock pelo memo (`memory.cpp:546`).
   - Os misses, `Query` e `QueryOverlap` ainda tomam o lock exclusivo.
   - Só vale se um perf mostrar contenção nesse `m_mutex`.
4. **`cb6ecb6b` — `RepairOrphanedProtection` (só se aparecer o sintoma).**
   - Rede de segurança: quando nenhum tracker reclama de um fault que o mapeamento do guest permite, devolve a proteção da página e tenta a instrução de novo.
   - Pode esconder erro de coerência, porque a GPU passa a ler dado velho sem ninguém saber.
   - Só portar diante de um "unhandled exception" num memcpy para memória de textura.
   - O `f7d71f91` (`IsAccessibleNow`) só funciona no Windows; no Linux o stub devolve `false`, então para nós ele não faz nada.

## Não portar

- `ac55a55e`, page table BDA de dois níveis:
  - A nossa tabela plana tem 512 MiB (40 bits, páginas de 16 KiB). Os dois níveis economizariam ~450 MiB de VRAM.
  - Em troca, põem uma leitura dependente a mais em todo acesso BDA de todo shader, o caminho quente que otimizamos em `a6037785`.
  - Com 16 GB na 9070 XT não compensa. Reavaliar só se a VRAM virar o limite.
- `078414c8`, `b1e92f46`, `097febee` (blocos VMA de 64 MiB) e `0ff7ed13` (quarentena de imagens): o autor voltou para 256 MiB por causa de DeviceLost (leitura de imagem já liberada).
- `498233e4` (`KYTY_BUFFER_TAIL_PAD_KB`): contorno de um caso do Wolverine.
- `9a9252a9`:
  - `KYTY_DEFAULT_SAMPLER_CLAMP` é um diagnóstico da lua repetida no céu do Wolverine.
  - A prioridade da thread da GPU sem efeito medido não muda nada para nós.
- A série de pesquisa de sampler bindless (`c9cc6306` → `c5f87095`, `c525f2e7`, `e5a69865`, `361130f9`, `9f04b85d`, `033432a9`, `92592e52`, `24f08ff1`, `0f2a4a83`, `f4b4db82`) e o contorno por título `95969286`.
- Diagnósticos de device lost, dumps e verificação:
  - `639d150a`, `ef62bbad`, `f33da6db`, `6bd09f2c`, `cf021f34`, `7de40300`;
  - `c946bd04` e `c60ddea6` (`KYTY_BDA_VERIFY`, ligado à tabela de dois níveis);
  - `5dcde26a`, `c2419014`, `973f5e6e`;
  - `04b365c3`, `4a0d7ff5` e `94f4e763` (SubgroupLocalInvocationId nos programas de relatório do watchdog).
- Documentação, README e empacotamento Windows (`fe1d8b1b`, `e41e5fa2`, `527df0ce`, `9c768414`).

## Pendente da análise de 2026-10-03

A política de GC do `wolverine-perf` ainda não foi adaptada: GC ciente de imagens, intervalo de 4 ms
e orçamento combinado. Nenhuma `KYTY_GC_*` ou `KYTY_IMAGE_RECYCLE_MB` existe no nosso código. Continua
sendo a ideia principal desse fork, com as ressalvas daquela análise (relógio por frames contra o
nosso por ticks; manter `MaintainHotPages`, faults e readbacks).

## Mais interessante que este fork: o Senaxx atual

`refs/senaxx/wolverine` (`48028863`, 2026-10-09) tem 58 commits desde a nossa integração de
2026-10-05. Vários tocam frentes nossas:

- `c5a6961b`, bindless pelo "engine method": as entradas da heap são traduzidas quando o guest as escreve. Relevante para a frente bindless/Yotei.
- `6632a241`, uma thread de scanner que prepara os walks de descritores à frente da Thread_Gpu.
- `2559c273`, janela de 4 MiB para readback; nós medimos 64 KiB com +32–44%.
- `402a830e` e `eb3f9128`, ISSUES #26/#27.
- `36fc5e01` e `3861fd94`, orçamento para placa de 16 GB.

Vale uma triagem própria.

**Atualização (mesmo dia):** a triagem do Senaxx foi feita em `docs/PORT-SENAXX-2026-10-10.md`
(tabelas por commit em `docs/port-senaxx-2026-10-10/`).
