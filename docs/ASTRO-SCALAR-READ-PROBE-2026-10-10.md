# Astro: diagnóstico de leitura escalar da travessia

O shader `7d6ab87e6c984bb7` foi identificado entre os comandos em voo nos
timeouts anteriores do Astro PPSA01325. Seu laço de lista termina quando
encontra um marcador negativo. Uma página BDA ausente ou leitura além do
descritor retorna zero, que não satisfaz esse marcador. O diagnóstico já
registrou uma leitura inválida real nesse laço; a relação causal com o timeout
da execução normal ainda precisa de uma comparação com a correção.

## Uso e contrato

`KYTY_SCALAR_READ_PROBE_SHADER=7d6ab87e6c984bb7` e
`KYTY_SCALAR_READ_PROBE_PCS=4fc,5a0` selecionam as leituras. O padrão é desligado.
`KYTY_SCALAR_READ_PROBE_DIR` seleciona a pasta de saída; o padrão é `_RTCapture`.
As opções participam do fingerprint para impedir reutilização de outro shader.

O primeiro acesso inválido registra hash, PC, descritor, endereço guest,
tamanho, offset, BDA e motivo: 1 limite do descritor, 2 fora da janela guest,
3 página ausente. O registro tem 80 bytes e preserva os 32 bytes iniciais do
trap e o bitmap de escritas BDA, que começa após a folga de 256 bytes.

Uma flag local e um voto de subgroup forçam saídas de laço já existentes,
preservando as arestas do CFG. O emissor recusa barreiras de workgroup,
dispatcher e waves virtualizadas. Laços sem saída condicional reconhecida
não têm garantia de recuperação. O host salva o registro após conclusão GPU
e invalidação do readback, então encerra intencionalmente o emulador por padrão.
O arquivo acompanhante `.faults.bin` preserva a lista de páginas ausentes do
mesmo lote: 1.024 `uint64_t`, sendo o primeiro o contador e os demais até
1.023 endereços. Contadores maiores indicam truncamento.

`KYTY_SCALAR_READ_PROBE_CONTINUE=1` permite continuar uma coleta após salvar
o trap e processar as páginas ausentes pelo caminho existente do cache.
Os arquivos recebem uma sequência para evitar sobrescritas. Essa opção é
exclusiva de diagnóstico: as saídas forçadas dos laços deixam incompleto o
resultado do dispatch afetado. Outros códigos de trap continuam fatais.
Este diagnóstico altera o trabalho com falha; não constitui correção, RT por
hardware, prova de imagem correta ou medição de desempenho.

## Validação

A fixture reproduz uma lista com marcador, dados ausentes, limite curto,
endereço não canônico, leituras no continue e seletores que não correspondem.
O caso saudável precisa encontrar o marcador; os casos de falha precisam
registrar os valores exatos, sem fabricar o marcador.

O primeiro teste falhou sem a implementação. A primeira versão de retorno
direto passou na fixture simples, mas o shader real falhou na validação de
pós-dominância do continue. Foi substituída pela flag e pelas saídas existentes;
a fixture ganhou casos de leitura no continue.

Dez casos passaram na Radeon com validação Vulkan `full` (zero erros/avisos)
e `sync` (zero erros; um aviso de configuração da camada). O teste CPU do
fingerprint/cache também passou. Comandos de teste:

```sh
KYTY_TEST_VULKAN_VALIDATION=full _Build/linux-clang/shader_recompiler_compute_tests --scalar-read-probe-only
KYTY_TEST_VULKAN_VALIDATION=sync _Build/linux-clang/shader_recompiler_compute_tests --scalar-read-probe-only
_Build/linux-clang/shader_recompiler_compute_tests --program-cache-only
```

O Astro sem patch pré-compilou 305 shaders sem falhas na execução
`runs/20261010-111057`. O módulo real instrumentado, extraído do cache com
checksums conferidos, passou no SPIRV-Tools para `vulkan1.2`: 56.175 palavras,
SHA-256 `cadd1b5407920b791a68c81831fa0b1e8d41e9c4dccce3a837a092c7006a7e79`.

O usuário confirmou imagem normal no início da observação. A execução terminou
com código 0, sem registro do probe e sem novo timeout/reset GPU no kernel.
Uma amostra intrusiva de dez segundos no GDB registrou 1.084 dispatches diretos
e 14 hashes, sem o shader alvo. Isso não cobre toda a execução nem dispatches
indiretos. A falha não foi reproduzida e a causa continua aberta.

Os artefatos e o manifesto estão em
`_Build/rt-integration-20261010/scalar-read-probe/`. O executável instalado foi
preservado e o launcher normal restaurado após a execução.

## Leitura inválida reproduzida

Na execução `runs/20261010-121913`, sem patch, o probe encerrou o emulador
intencionalmente com código 65. Salvou o seguinte registro de 80 bytes:

| Campo | Valor |
| --- | --- |
| Shader / PC | `7d6ab87e6c984bb7` / `0x4fc` |
| Motivo | 1: limite do descritor |
| Descritor | `[03aac000,00080004,00000000,00016204]` |
| Base / endereço / offset | `0x403aac000` / `0x403aac004` / 4 |
| Tamanho calculado | 0, pois `NumRecords=0` |
| SHA-256 do registro | `dab90f69cb95489e47dee94fb69501681558ccc4eb77d60276f99d99790d00e9` |

`BDA=0` nesse registro não prova falta da página da lista: o teste de limite
falha antes da consulta à BDA. Também não prova erro no cálculo do limite.
No shader, o tamanho da lista vem de `s42`, carregado do cabeçalho BLAS+88
pela instrução em `0x410`. Uma leitura anterior sem página pode zerar esse
valor; essa origem precisa ser conferida.

A execução `runs/20261010-123424`, com continuação diagnóstica, reproduziu
**o mesmo registro, byte a byte**. O arquivo acompanhante contém 16 páginas
ausentes, sem truncamento, incluindo `0x403aac000`, que contém o cabeçalho
BLAS+88. Isso correlaciona o limite zero com falta de residência no mesmo lote;
não identifica sozinho qual invocação ou leitura produziu o zero.

A captura BVH dessa execução salvou 64 registros em `0x9e8`, todos nós de
caixa tipo 5 com status 3 (página ausente). O auditor retornou 77, com
`queries=0` e `skipped_guard=64`: **não houve comparação com hardware RT**.
A confirmação do usuário de imagem normal não garante equivalência de todos
os resultados produzidos pelo modo diagnóstico.

Artefatos: `_Build/rt-integration-20261010/scalar-read-probe/dispatch-observation/`,
pastas `bvh-20261010-121913-N7hv1m` e `bvh-20261010-123424-QLEDPK`.
Esta última contém `manifest.json`, `scalar-reports.json`,
`capture-report.json`, os registros binários e a cópia dos logs/cache em
`game-run/`. A amostragem foi depois espaçada e adiada por 16 dispatches
selecionados após o armamento para procurar dados residentes.

Com amostragem `65535` e espera de 16 dispatches, a execução
`runs/20261010-124155` salvou 23 registros residentes: 20 caixas em `0x9e8`
e três em `0x5d4`, sem triângulos. A captura não atingiu o limite de 64;
seu SHA-256 é `131b6c46fda3936b760ef1ad7923af3b5fc40a1a75595dd7386a57f8d41cc9e7`.
O mesmo trap voltou a ocorrer, com 13 páginas ausentes, incluindo a página
do cabeçalho. Após a captura, uma leitura de 768 bytes do backing CPU da BLAS
mostrou `NumRecords=116` em BLAS+88. Essa leitura posterior não prova quais
bytes a GPU viu no instante da falha. Foi preservada como evidência separada
em `bvh-20261010-124155-7UH4jD/blas-cpu-backing-after.{bin,json}`.

O log `ScalarReadProbe dispatch recorded` identifica os primeiros 16 comandos
do shader selecionado gravados pelo host, incluindo frame, tick e dimensões.
Esse log não comprova execução ou conclusão GPU; o trap e os registros BVH
fornecem a evidência GPU. As alterações de log e arquivo acompanhante não
introduzem comandos, barreiras ou mudanças na vida dos buffers.

## Comparação hardware com triângulos reais

A captura `bvh-20261010-125559-v7Dh4h`, da execução
`runs/20261010-125559`, foi direcionada a `0x520` com
`KYTY_BVH_CAPTURE_PC=1312` (decimal), máscara de amostragem 4095, capacidade
64 e espera de 16 dispatches selecionados após o armamento. O filtro usa
o campo opcional 16 do cabeçalho; zero mantém a captura de todos os PCs.
Um teste GPU com duas instruções falhou antes da implementação e passou
depois, selecionando 32 ou 20 invocações conforme o PC e zero para um PC
ausente, sem mudar os resultados do shader.

Foram salvos 64 registros residentes, todos em `0x520`, distribuídos pelos
tipos de triângulo 0/1/2/3 em 22/16/11/15 registros. A amostra contém 12
combinações distintas de descritor/nó, dez interseções finitas e oito dentro
do intervalo não negativo do raio. A capacidade foi atingida: é uma amostra
parcial, não uma captura completa da árvore. SHA-256:
`f66af13438bd188980c021a40695ed836996d86b59df583f9dc9e4dc2ea3c3f4`.
Não houve arquivo de trap escalar nessa execução até a coleta.

Após encerrar o Astro, o auditor executou **64 consultas nativas na RX 9070
XT**, sem registros ignorados, com zero divergências de acerto, distância
e coordenadas baricêntricas. A tolerância relativa de distância é `2e-5`;
a de coordenadas baricêntricas é `1e-4`. A validação Vulkan `full` registrou
zero erros e zero avisos, e o processo retornou 0. Os logs e o CSV estão
na pasta da captura, em `hardware-replay.log` e `hardware-witness.csv`.

O módulo real com o filtro passou no SPIRV-Tools para `vulkan1.2`:
58.486 palavras, 11 laços, 48 shuffles, SHA-256
`5ba0e4bdfc90787c434838e20573c9f58704d4b8c5a63dc45cbbd78cc469a316`.
Os 636 registros do cache extraído tiveram seus checksums conferidos.
O build canônico, a suíte de captura, os dez casos do probe e os testes CPU
de cache/fingerprint passaram após a alteração do filtro.

Isso valida a conversão e a interseção de folhas individuais com dados do
jogo. Não substitui a travessia do Astro, não valida instâncias, listas ou
regras de aceitação do shader e não demonstra ganho de FPS. A integração
depende ainda de um snapshot coerente da BVH completa e dessas regras.
