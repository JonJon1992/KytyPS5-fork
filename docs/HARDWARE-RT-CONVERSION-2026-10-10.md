# RT hardware: conversão de geometria BVH32 e cache de preparação

Etapa posterior: [backend integrado ao renderer](HARDWARE-RT-BACKEND-2026-10-10.md).
O relato abaixo documenta a conversão e o protótipo anteriores à integração.

Esta etapa implementa uma base testável para o backend de hardware RT.
**Não habilita RT por hardware nos jogos.** O código só é usado pelo teste
dedicado e pelo protótipo isolado; não há substituição no recompiler/runtime.

## Implementação

- `src/graphics/host_gpu/renderer/rt/guestBvh.{h,cpp}` recebe uma cópia coerente,
  imutável, dos bytes guest. O chamador precisa garantir que os produtores CPU
  e GPU terminaram antes de produzir essa cópia.
- Converte subárvores BVH32 com caixas FP32/FP16 e os quatro tipos de triângulo.
  Preserva ponteiro guest e flags por primitiva para posterior remapeamento.
- Limita a travessia e verifica descritor, aperture de 40 bits, intervalo dos
  nós e bytes disponíveis. Rejeita tipos desconhecidos, instâncias, ciclos,
  ponteiros repetidos e posições/limites não finitos. Uma falha remove qualquer
  geometria parcial. Não é um verificador de equivalência da árvore.
- Cache de uma entrada, de uso por uma thread: bytes exatamente iguais permitem
  reutilização; mudança de posições gera indicação de update; mudança de
  descritor, endereço, topologia ou ordem de primitivas indica rebuild.
  Metadados podem mudar sem reconstrução da geometria. Cada resultado é
  imutável, inclusive quando quadros antigos ainda retêm uma referência.
- O limite de 64 MiB é para os bytes da cópia, não para toda memória alocada.
  Geometria e armazenamento da travessia têm custo adicional, limitado pela
  contagem de nós. Não existe cache de AS Vulkan nem gerenciamento de fences
  neste componente. A indicação de update exige um AS criado com suporte a
  update e sincronização apropriada por parte de seu futuro proprietário.

## Evidência

1. `guest_bvh_conversion_tests`, compilado no build canônico `_Build/linux-clang`:
   CTest `guest_bvh_conversion` passou (1/1). Casos incluem FP16, quatro tipos
   de triângulo, NaN/infinito, descritor desconhecido, endereço fora da aperture,
   memória truncada, ciclos, limite de nós, imutabilidade e invalidação do cache.
2. Mesmo teste compilado com Clang 22, `-Wall -Wextra -Werror` e
   AddressSanitizer/UndefinedBehaviorSanitizer: passou. Detecção de leaks
   desativada (`ASAN_OPTIONS=detect_leaks=0`); não é evidência de ausência de leaks.
   A tentativa inicial com GCC não linkou porque o runtime ASan estava ausente.
3. Protótipo em `_Build/rt-integration-20261010/probe/`, derivado do protótipo
   anterior: uma árvore sintética de 85 caixas FP32 e 256 folhas cobre os quatro
   tipos de triângulo. A geometria produzida pelo novo conversor alimenta
   BLAS/TLAS Vulkan, com construção e atualização após mudar Z de 2 para 3.
   Verifica ordem/remapeamento de primitivas e reutilização da preparação.
   Cada consulta valida 65.536 raios com resultados analíticos de hit/miss,
   distância, índice e baricêntricas; duas rodadas por estado, nos caminhos
   hardware e software linear. Passou na RX 9070 XT (RADV GFX1201), com
   validation e synchronization validation habilitadas e zero erros reportados.
   O driver emitiu seu aviso de implementação não conforme. Os tempos do log
   não são benchmark: o objetivo foi correção, sem exigir máquina ociosa.

Logs: `_Build/rt-integration-20261010/{build,ctest,sanitizer,gpu-conversion}.log`.
O stub inicial que sempre rejeitava falhou como esperado; isso demonstra que o
teste detecta uma implementação ausente, não uma regressão de RT já existente.

## O que falta para jogos

`IMAGE_BVH_INTERSECT_RAY` executa uma operação sobre um nó indicado pelo guest.
Uma consulta Vulkan percorre uma estrutura opaca inteira. Mesmo um resultado
correto neste fixture não autoriza trocar uma instrução pela consulta: ordem
dos filhos, limites, flags, política de aceitação e dados intermediários podem
ser observáveis pelo shader guest.

Faltam captura coerente das árvores reais, reconhecimento/prova de um caminho
de travessia substituível, tratamento de instâncias e formatos necessários ao
jogo, cache de AS ligado à invalidação das páginas e à vida útil GPU, integração
de descriptors e comparação de resultados com a execução guest. O protótipo
não preserva automaticamente flags de baricêntricas nem aceitação de bordas.
Não há ganho de FPS comprovado nem alteração do binário em execução do Astro.
