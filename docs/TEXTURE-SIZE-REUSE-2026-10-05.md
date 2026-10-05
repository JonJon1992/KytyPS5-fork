# Tamanho e alinhamento de texturas — 2026-10-05

Continuação da otimização documentada em [PM4-TILE-2026-10-05.md](PM4-TILE-2026-10-05.md).
A meta era reduzir o caminho size-only de aproximadamente 84 ns/textura para
50–70 ns, mantendo os resultados. A mediana final foi 37,261 ns no benchmark
original e 47,716 ns no par pitch + tamanho usado pelos descritores.

## Diagnóstico e implementação

`BuildTextureDescription`, em `renderer/pipeline/descriptors.cpp`, chama
`TileGetTexturePitch` e `TileGetTextureTotalSize` para texturas sem MSAA. O cache
de descrição existente usa os dwords do descritor, incluindo o endereço; imagens
distintas com o mesmo footprint ainda podem repetir o cálculo.

O caminho size-only já evitava materializar os mips. Restavam consultas de
formato/bloco/tail por chamada, uma busca por nível para encontrar o mip tail e
divisões inteiras para converter texels e alinhar as duas dimensões de cada mip.
A inspeção do código gerado confirmou essas divisões; as dimensões de elementos
e blocos vêm de potências de dois.

Em `tile.cpp`:

- Uma entrada por thread reutiliza os fatos imutáveis de geometria por
  `(formato, tiling, volume)`. São 80 bytes no objeto Clang/x86-64 verificado.
- Outra entrada por thread reutiliza size + alignment por
  `(formato, tiling, largura, altura, níveis, profundidade de volume, volume)`.
  São 36 bytes. Para arrays/cubemaps, armazena o tamanho de uma fatia e aplica
  o número de layers e a checagem de overflow em **cada chamada**.
- A busca do primeiro mip tail virou um cálculo direto com `bit_width` no
  caminho size-only. O layout completo mantém a busca original como referência.
  Para limite `2^k`, `ceil(value / 2^level) <= 2^k` equivale a
  `value <= 2^(k + level)`; o primeiro nível é
  `bit_width((value - 1) >> k)`, combinado com o limite de quantidade de mips.
- Conversões de elementos usam shifts e alinhamentos tiled usam máscaras.
  O caminho linear também evita dividir pelo tamanho do elemento e pelo
  alinhamento em cada mip. As adições continuam em `uint32_t`, preservando
  o comportamento de wrapping anterior.

São duas entradas fixas, sem alocação, hashing, locks ou dependência de endereço,
conteúdo, tick, dispositivo ou estado do guest. Consultas de layout recusadas não
substituem a geometria anterior. A profundidade de volume participa da chave;
linear continua tratando depth como quantidade de fatias, inclusive quando o
flag de volume está ligado. O cache não guarda recursos Vulkan.

As assinaturas públicas e `descriptors.cpp` foram preservados. O caminho MSAA
dos descritores continua usando suas funções próprias, cujos resultados também
entraram na comparação diferencial.

## Microbenchmark A/B

Release, Clang 18, WSL Ubuntu 24.04, CPU lógico 0. Mesmo teste e flags para as
duas versões, sem contadores de teste, ASan/UBSan ou LTO. Uma rodada de
aquecimento e sete amostras alternadas por versão. A baseline foi o `tile.cpp`
preservado **antes desta tarefa**, já com a especialização size-only anterior;
não é a versão antiga de 225 ns que construía todos os mips.

O host apresentou variação de carga durante a sessão. Os 83,973 ns históricos
não se reproduziram no A/B final: a mesma implementação anterior marcou
60,932 ns. As reduções abaixo usam exclusivamente o A/B final, evitando comparar
medições de sessões diferentes.

| Workload | Antes, ns/textura | Depois, ns/textura |
|---|---:|---:|
| Benchmark original, 32 extents | 60,932 | 37,261 |
| Mesmo footprint consecutivo | 64,699 | 6,102 |
| Mesma fatia, layers de cube variando | 62,593 | 6,038 |
| 32 extents, matriz longa | 62,494 | 35,789 |
| 4.096 extents, geometria constante | 73,499 | 39,611 |
| Formato/tiling mudando a cada chamada | 69,157 | 52,388 |
| Um mip, dimensões variando | 36,514 | 24,012 |
| Volume, dimensões e profundidade variando | 72,868 | 38,005 |
| Linear, dimensões variando | 69,617 | 54,907 |
| Layout completo dos mips | 137,181 | 111,266 |
| Pitch + tamanho, 32 extents | 81,579 | 47,716 |
| Pitch + tamanho, footprint consecutivo igual | 75,971 | 16,531 |

O ganho foi de **38,8%** no benchmark original e **41,5%** no par pitch + tamanho.
Footprints consecutivos iguais tiveram redução de 90,6% na chamada de tamanho.
O workload com formato/tiling alternando não depende de hits da geometria.

Benchmark original: R32Float, Standard64KB, seis layers, onze mips, extents de
1920×1080 a 1951×1111, 20.000 repetições. Checksum preservado:
`50806128640000`. A matriz longa usa 100.000 repetições de 32 chamadas por
cenário. Os cenários de descritores medem as duas funções de footprint, não a
resolução completa do descritor, uploads ou acesso ao texture cache.

Amostras do benchmark original, ns/textura:

- Antes: 58,894; 71,094; 60,083; 64,017; 58,814; 63,138; 60,932.
- Depois: 32,983; 54,911; 42,106; 31,977; 37,261; 46,825; 35,158.

## Validação

- O teste de reutilização falhou na baseline: 32 consultas de formato para
  32 extents. A versão otimizada faz uma consulta. O contador liga o
  `gpu_format.cpp` real com um símbolo renomeado; não instrumenta o benchmark
  nem substitui tabelas de formatos.
- CTest: `tile_hardware_reference`, `tile_size`, `tile_size_reuse` — 3/3.
- Fixtures literais cobrem formatos comuns e BC, tilings, arrays, volumes,
  mips, alinhamentos e MSAA de 1/2/4/8 samples. O teste de reutilização também
  verifica mudanças de formato, tiling, níveis, profundidade e isolamento entre
  threads, incluindo reaplicação de layers de cubemap.
- Comparação adicional do size-only com o layout completo ao redor de potências
  de dois, sete formatos, cinco tilings, 2D/3D e todos os níveis de 1 a 16.
- Comparação binária baseline/final: **115.920 combinações**, com **39.150 layouts
  válidos**. Foram examinados os valores de formato 0–183, sete tilings incluindo
  linear, cinco extents, 1/5/16 mips, 2D/3D e 1/6/12 layers. As recusas pelo API
  de layout também foram registradas; as APIs void fatais não são chamadas para
  formatos/tilings recusados.
- A mesma saída inclui 375 combinações de sizing/pitch/MSAA/DCC, com os cinco
  tamanhos de elemento, contagens de fragments válidas e inválidas e layers
  diferentes; inclui depth, stencil e HTile para os samples suportados.
- Todos os **63.089.280 bytes** das duas saídas são idênticos: tamanho,
  alinhamento, pitch, mips, offsets, padding e coordenadas de tail. SHA256:
  `5c6f56b6555f8182dee97bbc4ae57aa4e69d4d864b86b7910247e29f0c85fda8`.
- ASan + UBSan nos três testes CPU e na matriz binária, com `tile.cpp`,
  `gpu_format.cpp` e os testes instrumentados. Bibliotecas auxiliares não foram
  instrumentadas. A saída instrumentada mantém o mesmo SHA256.
- Unidades de produção `tile.cpp` e `pipeline/descriptors.cpp` compiladas com
  Clang 18 e suas flags reais. Há dois avisos de retorno preexistentes em
  `descriptors.cpp`/`imageView.h`; estes arquivos não foram alterados.

A verificação diferencial e a compilação usam cópias dos fontes em um diretório
nativo temporário do WSL, conferidas por SHA256, com `-Werror=null-character`.
Isso evita aceitar avisos de leitura de NUL do volume Windows compartilhado,
observados anteriormente nesta sessão.

## Reprodução e alcance

```sh
cmake --build <build> --target tile_size_tests tile_size_reuse_tests tile_hardware_reference_tests
ctest --test-dir <build> --output-on-failure -R '^(tile_size|tile_size_reuse|tile_hardware_reference)$'
<build>/tile_size_tests --bench
<build>/tile_size_tests --bench-matrix
<build>/tile_size_tests --snapshot texture-sizes.bin
```

Para um A/B, ligar o mesmo teste e `gpu_format.cpp` com os objetos de `tile.cpp`
antes/depois, mantendo flags, CPU e workload; comparar os arquivos de snapshot
byte a byte. Os artefatos locais desta sessão estão em `_Build/texture-results.json`,
`_Build/tile-size-84-before.cpp`, `_Build/tile_size_verify.py` e
`_Build/tile_size_sanitize.py` (diretório ignorado pelo Git).

O benefício está no cálculo CPU de footprint ao resolver descritores de imagens
sampled/storage, especialmente entre imagens distintas com geometria igual.
Os resultados de MSAA foram preservados, mas o ramo MSAA não usa a nova memoização.
Não foi medido FPS/frame time no jogo nem executado um build completo ou a suíte
GPU nesta tarefa. Os tempos são de funções CPU e não representam ganho de FPS.
