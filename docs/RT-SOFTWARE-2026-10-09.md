# RT em software — 2026-10-09

## Resultado

Frente 11 do roteiro Radeon/RDNA: investigada a tradução de nós BVH em
`spirvEmitterBvh.cpp`. Nenhuma otimização de produção foi mantida nesta rodada.
Foi corrigido o preparo dos testes de BVH, que gerava escritas sobrepostas na
tabela de páginas. Não houve medição de FPS nem alteração do emulador instalado.

## Experimento

Os nós FP32 ocupam 128 bytes e o código traduz separadamente suas duas metades
de 64 bytes. A candidata reaproveitava a primeira tradução quando o nó inteiro
estava na mesma página de 16 KiB e a primeira tradução era válida. Mantinha a
segunda consulta para páginas distintas ou primeira página ausente, preservando
o caminho de falhas. Não alterava a matemática das interseções.

A candidata passou nos sete casos GPU e nos dois testes CPU/SPIR-V focados.
Os casos GPU incluem triângulos, caixas FP16/FP32, ordenação, baricêntricas,
raios inválidos, crescimento de limites e nó FP32 cruzando páginas com backing
físico descontínuo. Não foi acrescentado teste específico de página ausente.

Comparação da ISA produzida pelo RADV na Radeon RX 9070 XT, com cache Mesa
desabilitado e a mesma fixture:

| Caso BVH | Instruções antes | Candidata | Bytes antes | Candidata |
|---|---:|---:|---:|---:|
| TriangleIdUnsorted | 1206 | 1246 | 7304 | 7520 |
| TriangleIdSorted | 1283 | 1323 | 7868 | 8084 |
| BarycentricUnsorted | 1235 | 1275 | 7460 | 7676 |
| BarycentricSorted | 1309 | 1349 | 8004 | 8220 |
| BoxExitGrowth | 1290 | 1330 | 7888 | 8104 |

Cada variante ganhou 40 instruções estáticas e 216 bytes de ISA. O SPIR-V
também cresceu 216 bytes. Os dois casos FlatStack permaneceram iguais.
A contagem vem das linhas de instruções do disassembly, incluindo as palavras
literais para o tamanho em bytes; não é contagem dinâmica nem medida de VGPR.

Eliminar uma consulta executada não garante ganho quando exige mais desvios e
instruções. Sem medição temporal representativa de traversal, o aumento de ISA
não prova regressão, mas os resultados também não justificam ativar a candidata.
Ela foi retirada; o patch experimental está nos artefatos locais para eventual
A/B futuro. A implementação atual já compartilha a função de interseção entre
chamadas do shader e implementa triângulos; descrições antigas de triângulos
sempre retornando miss não caracterizam o código atual.

## Correção mantida

`tests/ShaderRayTracingGpuTests.inc`: a primeira entrada de `bda_mappings`
usava tamanho implícito até o fim do backing e alcançava a segunda página.
A segunda entrada voltava a escrever essa página com outro endereço físico,
sem sincronização entre os dois `vkCmdUpdateBuffer`.

O baseline original reproduziu cinco `SYNC-HAZARD-WRITE-AFTER-WRITE`, um por
variante BVH, apesar dos resultados numéricos corretos. Os tamanhos agora são
explícitos: 16 KiB na primeira entrada e 64 bytes na segunda. Isso preserva o
teste de páginas descontínuas e elimina a sobreposição acidental no preparo.
Baseline corrigido e candidata passaram com zero erros de validação de
sincronização, dois avisos de ambiente/configuração em cada execução.

## Evidências e reprodução

Artefatos locais em `_Build/rt-software-20261009/`:

- `before.log`: baseline original com os cinco erros de sincronização;
- `baseline.log`, `baseline-spv/`, `baseline.tested`: baseline com fixture corrigida;
- `candidate.log`, `candidate-spv/`, `isa-summary.json`: comparação experimental;
- `same-page-candidate.patch`: alteração de produção retirada;
- `cpu.log`: testes CPU/SPIR-V da candidata;
- `build-final.log`, `final-gpu.log`, `final-cpu.log`: verificação da versão final.

Build canônico: alvos `shader_cfg_tests`, `shader_recompiler_compute_tests` e
`kyty_emulator` em `_Build/linux-clang`, toolchain Clang de
`/tmp/kyty-clang-tools`.

Teste GPU: `shader_recompiler_compute_tests --wolverine-instructions-gpu-only`,
com `KYTY_TEST_VULKAN_VALIDATION=sync`. Captura de ISA/SPIR-V usa adicionalmente
`RADV_DEBUG=shaders`, `MESA_SHADER_CACHE_DISABLE=true` e
`KYTY_SPIRV_SNAPSHOT_DIR` apontando para um diretório distinto por execução.

Testes CPU: CTest filtrado por
`^wolverine_(instructions_cpu|instruction_fixtures_cpu)$`.

Após retirar a candidata, os três alvos recompilaram e a versão final passou
novamente nos dois testes CPU/SPIR-V e nos sete casos GPU, com zero erros de
validação de sincronização. `git diff --check` também passou.

A próxima frente do roteiro é hardware RT. Habilitá-lo exige avaliar equivalência
com os nós guest e custo de conversão; esta rodada não demonstra benefício de
hardware RT nem conclui que RT em software não possa ser otimizado.
