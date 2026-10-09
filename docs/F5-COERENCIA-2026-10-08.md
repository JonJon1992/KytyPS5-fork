# F5 — settle BDA adiado

Data: 2026-10-08. Base: F3 `57f0d97`. Worktree:
`/home/jonathanbraga/kyty-coherence`, branch `cpu-coherence-service`.
Estado: implementação em opt-in concluída e revisada em 2026-10-09.
Build Release aprovado; 53 de 54 testes passaram. A única falha também
ocorre na base F3. A/B no jogo permanece pendente.

## Entrega e fluxo

`KYTY_BDA_WRITES=deferred` remove o settle síncrono após cada dispatch
compute com raw DWORD stores BDA admitidos. Exige whitelist explícita em
`KYTY_BDA_WRITES_SHADERS`. Defaults e rejeições de stores não suportados
permanecem. Os dois shaders com prova F4 usam candidatos, sem bitmap.

1. O owner captura as faixas dos buffers BDA registrados, confere mappings
   e aliases físicos únicos e preserva/uploads/proteção antes do writer.
2. Após o último preparo de bindings, abre um ticket com Native tick,
   geração e domínio retidos; reserva ownership e versões nesse tick.
3. Grava writer e compactação, sem esperar imediatamente pela GPU.
4. Runner independente aguarda Native, invalida o download e lê o bitmap.
   Não aplica metadata de cache nem aguarda o Thread_Gpu.
5. O Thread_Gpu recebe um wake interno, valida cobertura e fecha Applied.
   Não há uploads, preservação ou troca de ownership tardios.
6. Labels, EOP, interrupções, flips e retirement capturam o prefixo Applied.
   Uma publicação intermediária sela seu Native tick para não esperar
   writers posteriores. Writes posteriores ao mesmo label suspendem.

CPU espera somente produtores da faixa acessada; o readback não amplia a
janela por cima de outra pendência. O CP consulta readiness antes de ler
comandos, argumentos, índices, SRT e tabelas de vértices. Suspende antes de
emitir e permite outras filas avançarem. Multi-draw conserva o próximo
registro; DrawPrep conserva o head até o retry concluir.

Epoch muda na abertura e no fechamento de unknown writes. Certificados do
DrawPrep, fills e verdicts não reutilizam provas antigas. Probes clean,
inclusive batch e helpers, recusam labels pendentes mesmo fora do domínio.

## Custos e limites

- Ring de oito produtores e oito downloads alocados no primeiro uso.
  Domínios mantêm capacidade entre jobs; nenhuma alocação nas consultas.
  Backpressure antes de admitir o nono produtor drena trabalho anterior.
- A cobertura começa com busca binária no domínio ordenado e percorre somente
  as interseções. O reset do download faz flush apenas dos contadores
  (8 + 4 bytes); o VMA arredonda para o alinhamento não coerente necessário.
- A compactação permanece por writer nesta entrega. Ela ainda percorre o
  bitmap na GPU; agrupá-la por submissão é trabalho posterior.
- O domínio inicial cobre todos os buffers BDA registrados. Isto pode
  aumentar preservação e upload. O ganho líquido precisa ser medido.
- Overflow da lista mantém todo o domínio GPU-owned. Escrita descartada ou
  página fora do domínio é erro de cobertura e impede conclusão válida.
- Alias físico existente recusa admissão. Map de novo alias, unmap/remap e
  shutdown drenam referências antes de expor/liberar a geração.
- Nenhum novo worker de cópias; collector não ocupa a FIFO da F1 e não
  substitui o hook pré-submit usado pela oclusão.

## Validação

Artefatos em
`/home/jonathanbraga/KytyPS5-fork/_Build/coherence-f1/validation/`:

- `f5-final-build.log`: build Release do emulador e dos alvos de teste.
- `f5-final-tests.log`: 53/54 testes passaram em 26,89 s. Todas as variantes
  nativas F5 passaram, incluindo recorder 0/1, CP inline/thread e scheduler.
  As regressões focadas de F1–F4 também passaram.
- `f5-resource-baseline.log`: o único teste que falhou, `resource_tracking`,
  reproduz a mesma assertion de FMASK (`index >= args.size()` em
  `ir/Value.cpp:188`) na base F3 `57f0d97`. O executável de comparação recompila
  as 11 unidades do alvo e os headers alterados a partir dessa base; reutiliza
  somente o archive comum cujas fontes não mudaram.

A revisão fresca foi encerrada sem achados materiais restantes após corrigir
readiness, retomada do CP, certificados e guards de labels. A falha FMASK
preexistente fica registrada; esta entrega não corrige o caminho FMASK.

A fixture Vulkan exercita a compactação real, barriers e invalidação de
host. Escritas de teste/bitmap são gravadas por transfer no native buffer;
a fixture não reproduz o programa inteiro de um jogo.
Matriz expandida: A/B sobrepostos com semáforos; publicação A independente
de B; CPU raw read; writer conhecido posterior; COND_EXEC; outra fila;
readiness de shader e tabelas de vértices; retry do DrawPrep; label em
página clean; alias/unmap; overflow/drop e reuso do slot do coletor. O gate
fatal do owner para dropped/out-of-domain foi revisado; a fixture negativa
verifica os contadores e o fallback de overflow, sem executar esse fatal.

## Medir no jogo

Mesmo ponto, personagem/câmera parados, caches aquecidos, 30 segundos por
execução. Comparar o mesmo conjunto de shaders/draws, sem mudar flags de
bindless, resolução ou cena. Não comparar FPS de um shader pulado com FPS
após habilitá-lo.

Registrar FPS mediano, frame time p95, CPU Thread_Gpu e
`FrameWait.BdaSettle.{Calls,Nanoseconds}.Cumulative`,
`FrameEvent.BdaSettles.Cumulative`, `FrameEvent.BdaSettlePages.Cumulative`,
`CoherenceCopyGuardWaits` e `CoherenceCopyGuard`. O caminho deferred não
registra a espera síncrona BdaSettle imediatamente depois do writer;
faults CPU e saturação do ring ainda podem esperar produtores.

Para ativar em um comando existente, passar como argumentos do wrapper:

~~~text
KYTY_BDA_WRITES=deferred KYTY_BDA_WRITES_SHADERS=<hashes raw DWORD suportados>
~~~

O wrapper `tools/run-u59.sh` limpa variáveis do ambiente; essas flags precisam
ser argumentos. O binário é `_Build/coherence-f1/kyty_emulator` na árvore
principal. Com somente os dois hashes F4, o modo deferred prefere candidatos
e não mede o collector genérico da F5. Nenhum ganho de FPS atribuído à F5
até concluir A/B com trabalho equivalente.
