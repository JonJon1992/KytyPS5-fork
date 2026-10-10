# Astro: diagnóstico de leitura escalar da travessia

O shader `7d6ab87e6c984bb7` foi identificado entre os comandos em voo nos
timeouts anteriores do Astro PPSA01325. Seu laço de lista termina quando
encontra um marcador negativo. Uma página BDA ausente ou leitura além do
descritor retorna zero, que não satisfaz esse marcador. A participação desse
mecanismo na falha real ainda não foi confirmada.

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
e invalidação do readback, então encerra intencionalmente o emulador.
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
