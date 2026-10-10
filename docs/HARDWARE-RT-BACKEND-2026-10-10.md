# Backend Vulkan de RT integrado ao renderer

## Estado

O backend experimental agora pertence ao `RenderContext` e usa o dispositivo,
VMA, command buffers e scheduler do emulador. Não cria uma segunda instância
ou dispositivo Vulkan, como os protótipos isolados anteriores.

**A integração para executar a travessia dos jogos em hardware continua
incompleta.** Não existe ainda uma substituição no recompiler nem um chamador
de `Prepare`/`Trace` nos dispatches guest. O acesso é pelo
`RenderContext::GetHardwareRt()` e pelos testes integrados. Habilitar a
capacidade do backend não muda os shaders do Astro e não constitui ganho de FPS.

## Implementação e contratos

- `RT::DeviceFeatures` consulta extensões, buffer device address,
  acceleration structure e ray query antes de habilitar as features. A criação
  de dispositivo do emulador e o harness de testes usam esse mesmo código.
  `KYTY_HW_RT_BACKEND=1` habilita a capacidade quando suportada; o padrão é off.
  Features opcionais retornadas pela consulta não são habilitadas implicitamente.
- `HardwareBackend::Prepare` recebe o snapshot coerente definido em
  `guestBvh.h`, converte a geometria e constrói BLAS/TLAS na fila existente.
  A origem dos bytes continua sendo responsabilidade do chamador: não há leitura
  automática de backing guest possivelmente desatualizado.
- Snapshots idênticos reutilizam a cena. Mudança de metadados preserva o AS
  e cria uma nova visão dos identificadores. Geometria diferente cria outro AS:
  **o backend integrado ainda não faz update in-place**. Isso preserva consultas
  anteriores pendentes e evita alterar memória que a GPU ainda lê.
- `Trace` grava RayQuery em um shader compute com buffers por consulta e
  descriptors push. Valida os raios antes de enviá-los à GPU. Os resultados são
  distância, baricêntricas nativas e índice da primitiva; `Primitives` oferece
  os ponteiros/flags guest correspondentes. Não são os quatro resultados crus
  de `IMAGE_BVH_INTERSECT_RAY`.
- O scheduler retém cada cena e consulta até sua conclusão, mesmo se o chamador
  liberar sua referência cedo. `Read` não bloqueia nem expõe resultados antes
  da conclusão; publica o readback com release/acquire. As leases externas
  precisam ser liberadas antes de destruir seu `RenderContext`.
- O construtor do backend é privado e só o `RenderContext` pode criá-lo.
  A revisão encontrou e eliminou a possibilidade de destruir um backend
  independente enquanto pipelines ainda eram usados por consultas pendentes;
  há uma asserção de compilação que impede reabrir essa construção externa.
- As dependências cobrem host → build, BLAS → TLAS, build → compute e
  compute → host. Buffers não coerentes passam por Flush/Invalidate. O scratch
  é alinhado à propriedade do dispositivo. A gravação nativa usa `Handle` e
  invalida o estado de descriptors compute para não contaminar binds guest.
- Os limites de bytes/nós da preparação e os limites de primitivas, workgroups
  e alocações do backend evitam pedidos sem limite. Os limites por alocação
  não são um orçamento global: leases retidas por chamadores também consomem
  memória. A política de OOM do Buffer/VMA continua a do emulador.

## Testes executados

Build canônico Clang de `kyty_emulator` e `shader_recompiler_compute_tests`
concluído. Artefatos em `_Build/rt-integration-20261010/`:

| Execução | Evidência |
|---|---|
| `--hardware-rt-only`, validation=sync | Passou, zero erros Vulkan |
| `--hardware-rt-disabled-only`, validation=full | Passou, zero erros e warnings da camada |
| `--hardware-rt-only`, recorder=1, validation=full | Passou, zero erros; ownership sem falhas |
| `--hardware-rt-only`, recorder=1, verify=exit, validation=sync | Passou, zero erros; 12 verificações, zero mismatches e ownership faults |
| `--hardware-rt-only`, barrier batch=0, validation=sync | Passou, zero erros; recorder se desativa nessa configuração por contrato existente |
| `--wolverine-instructions-gpu-only`, validation=sync | Casos existentes de BVH/FLAT passaram, zero erros |
| CTest conversão BVH e dois testes CPU Wolverine | 3/3 passaram |

O modo `sync` emite um warning de configuração porque desliga validação de
SPIR-V; `full` mantém validação de shaders, mas não liga synchronization
validation neste harness. Foram executados separadamente. O driver também
emite seu aviso RADV de implementação não conforme.

O teste integrado cobre os quatro tipos de triângulo, hit/miss, t/baricêntricas,
IDs guest, workgroup parcial (65 raios), cache, metadados, cenas diferentes no
mesmo command buffer, retenção de consultas liberadas cedo, Clear, NaN e tipo
de nó não suportado. O primeiro teste detectou uso incorreto de `BeginCommand`
com gravação ativa; foi corrigido para `Current` e os testes acima foram
executados após a correção. Não foi executada a suíte completa nem benchmark.

Os modos também estão registrados como `hardware_rt_backend_gpu` e
`hardware_rt_backend_off` no CTest, com labels `gpu;hardware-rt`. O primeiro
retorna 77 (skip, não aprovação) quando faltam features/extensões de RT.
Ainda falta um teste de intercalamento de dispatch guest → RT → dispatch guest;
a invalidação de estado foi revisada no código e os testes atuais intercalam
consultas RT, não shaders de um jogo.

Após a correção de ownership, o build foi refeito e os cinco testes CTest
(conversão, dois CPU Wolverine, backend GPU e backend off) passaram: **5/5,
nenhum skip**. O teste GPU final usou recorder em outra thread, verificação de
ownership em modo exit e synchronization validation: zero erros Vulkan,
zero mismatches e zero ownership faults. Registro em `runtime-final-ctest.log`;
hashes finais em `runtime-manifest.json`.

## Travessia do Astro: impedimento concreto

O journal já capturado permitiu extrair 13 shaders contendo BVH sem alterar o
jogo. O shader `0x7d6ab87e6c984bb7` contém 1.673 instruções, 132 blocos no CFG
original e dez laços. A decodificação está em
`_Build/rt-integration-20261010/astro-sources/` e a análise em
`astro-traversal.log`.

Ele invoca a instrução BVH em PCs `0x520`, `0x5d4` e `0x9e8`, com descritores
distintos. Em torno de `0x4d8` e `0x4fc` consulta uma tabela adicional de
ponteiros/metadados. Em `0x538..0x584`, filtra resultados por flags, sinal e
distância e preserva identificadores vindos dessa tabela. Uma consulta Vulkan
nearest-hit opaca como a do backend não reproduz automaticamente esses passos.

Para passar a executar esse percurso em hardware, faltam a captura coerente
dessas estruturas reais, a conversão de sua organização de folhas/instâncias,
a preservação da política de aceitação e um reconhecimento validado do trecho
de shader substituível. A evidência sintética não autoriza contornar essas
etapas. O jogo continua usando o caminho software existente.

## Captura de instruções executadas

Foi acrescentada uma instrumentação opcional do recompiler para registrar os
argumentos e os quatro resultados crus de cada instrução BVH executada. Ela
preserva o resultado software e lê os bytes dos nós pelo mesmo mapeamento GPU
usado na interseção. Não é uma captura completa de cena ou um atalho para
substituir a travessia.

`KYTY_BVH_CAPTURE_SHADER=<hash hexadecimal>` seleciona um shader; `all`
seleciona shaders com BVH. `KYTY_BVH_CAPTURE_TRIGGER=<arquivo>` permite armar
a coleta apenas quando o arquivo existe. `KYTY_BVH_CAPTURE_DIR=<diretório>`
seleciona a saída. A opção é desabilitada por padrão e faz parte da chave de
compilação/cache. Não há mudança no layout de descriptors guest.

O buffer privado comporta até 65.536 registros de 256 bytes. O contador atômico
satura nesse limite. O arquivo inclui shader, tick, dimensões de dispatch,
PC guest, identificação da invocação, descritor, nó, raio, resultado e bytes
da folha/caixa. No formato v2, chamadas rejeitadas também são registradas com
status de entrada inválida ou nó não mapeado, sem carregar memória física
inválida. Capturas vazias podem ser repetidas após a conclusão GPU, com limite
de 16 tentativas. Uma captura com registros encerra a coleta.
No modo `all`, um shader que produziu uma captura vazia concluída é excluído
das próximas tentativas dessa sessão, permitindo coletar shaders posteriores.
Na seleção de um hash específico, continuam permitidas tentativas posteriores
do mesmo shader.

O readback é publicado após a conclusão do scheduler e a invalidação de memória
não coerente. A lease do download e o estado de conclusão permanecem vivos até
o callback; ele não captura o `FaultManager`. Dispatches indiretos copiam as
dimensões do buffer GPU utilizado pelo próprio dispatch.

O primeiro build de captura foi instalado com backup e o Astro foi aberto sem
patch. O usuário confirmou imagem normal na mesma cena. A captura do shader
`7d6ab87e6c984bb7`, tick 234.089, grupos `[1, 2048, 1]`, produziu **zero
registros**. O arquivo de 256 bytes está em
`_Build/astro-rt-run/_RTCapture/20261010-013947/`. Esse resultado não demonstra
execução de interseções, nem compatibilidade ou ganho de hardware RT. A coleta
v2 amplia o diagnóstico para outras instruções e dispatches.

A coleta inicial v2 retornou 16 arquivos vazios do shader
`10e28006353301ec`, dispatch indireto `[1, 1, 1]`. Isso expôs uma falha da
seleção `all`: o primeiro shader despachado consumia todas as tentativas.
Uma fixture reproduziu essa falha, e a seleção passou a ignorar os shaders
cuja captura vazia já terminou. Os arquivos originais e o relatório ficam
preservados em `astro-capture-v2-empty.json` para não confundir ausência de
amostras com validação de RT.

Antes desse ajuste de seleção, os testes v2 de captura passaram com
`validation=sync` e `full`, incluindo entradas inválidas/não mapeadas, wave32,
wave64, identificação 2D, capacidade/guard, readback direto/indireto, shutdown
pendente e transições vazio → preenchido → parada. O auditor comparou 64 raios
da fixture GPU com folhas nativas: zero diferenças de hit, distância e
baricêntricas dentro das tolerâncias, e detectou separadamente as três
referências corrompidas. Os 20 casos BVH/FLAT existentes também passaram. Não
é evidência de paridade da travessia real do Astro.

Depois da correção, oito shaders distintos ainda produziram capturas vazias.
Ao alcançar o shader principal `7d6ab87e6c984bb7`, tick 409.890, foi publicado
um arquivo com 65.536 registros no PC `0x9e8`: todos tinham status `Unmapped`,
nó `0x25` (raiz box32) e descritor
`[0403b286, 80000000, 00000817, 81000000]`. Os raios eram finitos, mas nenhum
registro continha bytes de nó. Isso mostra execução da instrução e ausência
de mapeamento naquele dispatch, sem estabelecer o formato da geometria real.

O jogo encerrou em seguida com `ErrorDeviceLost`. O kernel registrou timeout
na fila gfx às 02:17:06, associado ao processo do emulador. Não há diagnóstico
que atribua a causa exata à captura, à travessia ou a um acesso inválido. O
arquivo já estava publicado após sua conclusão GPU. O replay validou o
arquivo e retornou **77**, com zero consultas e 65.536 entradas rejeitadas;
isso é ausência de evidência de paridade, não um teste de RT aprovado.

A captura seguinte usa `KYTY_BVH_CAPTURE_RECORDS=4096`, máscara
`KYTY_BVH_CAPTURE_SAMPLE_MASK=4095` e
`KYTY_BVH_CAPTURE_SKIP=120` para o shader principal. A máscara seleciona
invocações por um hash determinístico de suas coordenadas antes de acessar
o contador/CAS. Quando desabilitada no cabeçalho, a instrumentação verifica
apenas `Enabled`, sem acessar o contador. Download e cópia são dimensionados
pela capacidade escolhida. O limite de formato continua sendo 65.536.

O campo `SampleMask` ocupa a palavra 15, anteriormente zero/reservada,
no mesmo cabeçalho v2; valor zero preserva a seleção de todas as invocações.
Um teste com máscara 3 exige 16 registros de 64 invocações, nos modos wave32
e wave64/2D, e preserva os resultados software das 64 invocações. Antes da
implementação, o teste falhou por receber 64 registros. Após a implementação,
os testes de captura passaram em `sync` e `full`, com zero erros Vulkan.
O replay GPU sintético e os 20 casos BVH/FLAT também passaram novamente.
Logs: `capture-sampling-{sync,full,native-full,software}.log`.

O teste real desse build voltou a encerrar em `ErrorDeviceLost` às 02:32:36,
com timeout gfx registrado pelo kernel para o PID 294508. A pasta de coleta
estava vazia e o trigger ainda não existia: a instrumentação estava compilada
no shader, mas a coleta não tinha sido armada. Reduzir a capacidade e amostrar
invocações não resolveu a falha. Os testes sintéticos aprovados não validam
a estabilidade desse shader completo no jogo.

Os marcadores AMD mostram como primeira operação em voo um `DispatchDirect`
`[1, 2048, 1]`, modo 65, do shader `7d6ab87e6c984bb7`, tick 322.785. O código
guest salvo na falha tem o mesmo SHA-256 do dump analisado anteriormente.
`VK_EXT_device_fault` retornou sucesso, mas sem descrição, endereços ou dados
vendor; isso não identifica a causa exata do timeout. Não há prova de que a
captura, uma travessia infinita ou um acesso inválido seja a causa.

CppStudio state: Recovery; reason=timeout real persistiu com captura reduzida
e sem trigger; evidence=`capture-sampling-incident/incident.json`;
exit=comparação do mesmo build sem instrumentação e diagnóstico reproduzível.
A configuração de captura ficou suspensa para testes no jogo. O script
`_Build/astro-rt-run/start-bvh-capture.sh` encaminha para `start-rt-control.sh`,
que força `KYTY_BVH_CAPTURE_SHADER=0` e preserva o preset e a ausência de patch.
O backend hardware não estava ativo no jogo e continua desativado nesse
controle. A configuração anterior, os shaders da falha e a proveniência
ficam em `_Build/rt-integration-20261010/capture-sampling-incident/`.
O controle foi executado em `runs/20261010-024208`, com o mesmo SHA-256 do
emulador, `KYTY_BVH_CAPTURE_SHADER=0`, backend hardware desligado e sem
`--game-patch`. O usuário confirmou a mesma cena de gameplay e imagem normal.
O kernel voltou a registrar timeout gfx às 02:43:40 para o PID 305594; o
processo encerrou com código 65. O primeiro marcador em voo novamente era
`DispatchDirect [1, 2048, 1]`, modo 65, shader `7d6ab87e6c984bb7`, tick 235.571.
A observação planejada de três minutos não foi concluída. Esse controle
rejeita a captura como condição necessária para a falha; não estabelece a
causa exata. Evidências: `control-manifest.json`, `control-stability.json` e
`control-failure/` na pasta do incidente.

Os módulos SPIR-V efetivamente usados foram extraídos dos caches de programa
das duas execuções, com os checksums dos registros verificados. Ambos passaram
no validador SPIRV-Tools para `vulkan1.2`. O controle contém 54.636 palavras,
11.011 instruções, dez laços estruturados e 48 shuffles de subgroup. Ele não
contém os atômicos de captura nem `OpGroupNonUniformBroadcastFirst`. A chave
estática indica wave guest 64, subgroup host 64 e tamanho local `[128, 1, 1]`;
portanto a virtualização de wave64 sobre subgroup32 não explica esta execução.
Validação estática não prova terminação, acesso dinâmico válido ou estabilidade.

## Diagnóstico com Context7 e especificações Vulkan

O Context7 foi consultado usando `/khronosgroup/vulkan-docs`. A documentação
oficial foi confrontada com os módulos reais e as features da Radeon:

- [Subgroup uniform control flow](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_subgroup_uniform_control_flow.html)
  e [maximal reconvergence](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_maximal_reconvergence.html)
  fornecem garantias adicionais de reconvergência. O driver anuncia ambas as
  features, mas o emulador não as habilita nem emite seus modos de execução.
  Isso é uma hipótese a verificar, não uma causa demonstrada do timeout.
- [OpGroupNonUniformShuffle, SPIR-V](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpGroupNonUniformShuffle)
  exige que a origem pertença ao conjunto de invocações participantes naquele
  ponto. `EmitLaunchedLaneAtOrBelow` calcula um ballot no ponto da leitura;
  seu fallback devolve a lane pedida quando não encontra participante abaixo
  dela. Um conjunto divergente contendo somente lanes altas e uma leitura da
  lane 0 pode violar esse contrato. Ainda não foi demonstrado que esse estado
  ocorre no dispatch do jogo. Habilitar uma extensão sozinho não resolve uma
  leitura de origem ausente dentro de um ramo divergente.
- [vkGetDeviceFaultInfoEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceFaultInfoEXT.html)
  permite obter os diagnósticos disponibilizados pelo driver após device loss.
  As duas execuções retornaram sucesso com contagens zero; não há endereço de
  falha nem dump vendor que identifique a instrução causadora.

Foi adicionado apenas o seletor `--loop-guard-only` ao executável de testes,
para executar os testes existentes do diagnóstico sem os testes de fragment
não relacionados que abortam anteriormente em `--gi-probe-only`. O build do
alvo terminou com sucesso. Na Radeon, `LoopGuardCodegen` e
`LoopGuardEndlessLoop` passaram, com 64 relatórios de invocações limitadas e
zero erros ou avisos da camada Vulkan em modo `full`. Log:
`capture-sampling-incident/loop-guard-isolated-full.log`. Isso não significa
aprovação da suíte completa; o modo GI anterior encontrou erros em outro teste.

A revisão do CFG do módulo de controle confirmou que o guard existente pode
interromper todos os seus dez ciclos estruturados. Um teste real com orçamento
limitado pode separar laços muito longos de outras hipóteses, mas altera o
resultado de RT quando o orçamento esgota. O guard continua desabilitado por
padrão e não é uma otimização nem uma correção de estabilidade.
O contador do teste isolado foi lido após a conclusão GPU. No jogo, o logger
existente lê o GDS mapeado sem esperar nem invalidar; ausência de mensagem
não pode ser interpretada como zero esgotamentos. O shader deste diagnóstico
não usa GDS no caminho original, mas o report acrescenta seu último dword.

CppStudio state: Recovery; evidence=controle reproduziu o timeout sem captura;
blocker=causa do timeout do shader completo ainda desconhecida;
next=diagnóstico limitado de terminação, antes de retomar a captura de geometria.

## Pausa solicitada: resultado do diagnóstico de laços

Em `runs/20261010-030220`, o mesmo emulador foi aberto sem patch, sem captura
e sem backend hardware, com `KYTY_LOOP_GUARD=256` somente para
`7d6ab87e6c984bb7`. As opções foram conferidas no ambiente do processo
PID 325658. O usuário inicialmente confirmou a mesma cena e imagem normal,
mas enviou depois uma captura às 03:04:15 com quase toda a geometria preta.
O log registra 773 atualizações do contador, com último valor de 33.474.368
invocações que excederam o orçamento. Esse contador acumula invocações ao
longo dos dispatches; não representa o número de laços infinitos ou de
iterações de um único raio.

A sessão terminou com código 0 e sem `ErrorDeviceLost` no log. Quando se
tentou encerrar apenas essa instância, o processo já estava ausente; nenhum
sinal foi enviado pelo Codex. Não foi concluída uma observação comparável
de três minutos. A primeira leitura de existência do PID sob o sandbox não
enxergava o processo host e foi invalidada: não era evidência de crash.
Os logs e o cache de programas foram preservados em
`capture-sampling-incident/loop-probe/`; proveniência e limitações estão em
`loop-probe-manifest.json`.

O teste mostra que o orçamento corta trabalho efetivamente executado pelo
shader. A imagem incorreta impede tratá-lo como correção ou otimização;
também não distingue uma travessia válida longa de um defeito de terminação.
O limitador permanece desabilitado no preset normal. O script de controle
agora força explicitamente orçamento zero, e `start-loop-probe.sh` encaminha
para esse controle; o script original do diagnóstico está preservado junto
dos logs. Não houve mudança no código de execução do emulador nesta etapa,
somente o seletor isolado de testes e a documentação/configuração local.

Em 10/10/2026 o usuário pediu para documentar e parar, para continuar no dia
seguinte. A investigação está pausada a pedido do usuário. Retomar por:

1. Revisar o SPIR-V real do diagnóstico e localizar as condições de saída dos
   laços da travessia, sem usar um limite que altere o resultado como solução.
2. Verificar com uma reprodução pequena a participação da origem dos shuffles
   e a necessidade de reconvergência; as duas extensões Vulkan são suportadas,
   mas não foi provado que sua ausência cause o timeout.
3. Produzir uma correção sustentada por essa evidência e validar a imagem e a
   estabilidade na mesma cena, com captura e limitador desligados.
4. Retomar a captura coerente de nós residentes e os requisitos de conversão/
   travessia para integrar hardware RT ao shader real. O backend sintético
   continua separado desse caminho: não há ganho de FPS do jogo comprovado.

Nenhum build adicional, relançamento de jogo ou commit foi feito após o pedido
de pausa.

`tools/rt-bvh-capture.py` valida tamanho, limites, versão e status antes de
resumir os registros. O auditor `--bvh-capture-replay <captura> <csv>` usa o
backend nativo para comparar folhas individuais capturadas. Ele preserva os
vértices/flags, mas não reconstrói pais, instâncias, listas ou políticas de
aceitação do shader. Distância e baricêntricas usam tolerâncias explícitas;
a comparação não prova equivalência bit a bit ou travessia completa.

Nenhum patch do Astro foi reativado e nenhum commit foi feito nesta etapa.

## Retomada: comparação com o backup anterior

O usuário pediu para continuar em 10/10/2026. A pausa acima é histórica.
CppStudio state: Investigative; uncertainty=origem do timeout da travessia;
canonical proof=Astro PPSA01325 sem patch/captura/limitador na mesma cena;
next=separar semântica de lanes de disponibilidade dos dados de travessia.

O SPIR-V do diagnóstico anterior foi extraído do cache preservado, conferindo
os checksums. O módulo contém as dez condições do guard e passou no
SPIRV-Tools para `vulkan1.2`; o limitador estava presente de fato. Isso não
transforma a imagem preta em resultado válido.

`tests/ShaderTraversalLaneTests.inc` acrescenta uma reprodução pequena da
pilha em lanes: `V_WRITELANE` seguido de `V_READLANE` em laços, profundidades
32/64, com EXEC zerado, somente a metade alta e todas as lanes. São duas waves
por grupo. Os seis casos de `--traversal-lanes-only` passaram na Radeon com
validação Vulkan `full`, sem erros ou avisos da camada. O checksum esperado
é independente do recompiler. A fixture não contém o BVH completo nem prova
a participação das origens em todos os caminhos divergentes do shader real.
Log: `_Build/rt-integration-20261010/traversal-lanes-full.log`.

Foi então executado o backup **anterior à captura**, SHA-256
`541e3a03f0716dc327600677270b1544c4db75f5e538d33172962ce3c10d376f`,
em `runs/20261010-102326`, PID 14088. Esse é o mesmo binário da observação
histórica com imagem normal em `runs/20261010-002259`; aquela observação
não era garantia de estabilidade. O volume dos jogos foi montado pelo usuário.
O executável e o ambiente foram conferidos no processo host: sem patch,
`KYTY_BVH_CAPTURE_SHADER=0`, `KYTY_HW_RT_BACKEND=0`, `KYTY_LOOP_GUARD=0`.

O usuário confirmou imagem normal, mas o backup também terminou com código
65. O kernel registrou timeout `gfx_0.0.0` às **10:24:32 -03:00**. O primeiro
marcador em voo era novamente `7d6ab87e6c984bb7`, `DispatchDirect [1,2048,1]`,
modo 65, tick 157604. `VK_EXT_device_fault` retornou sucesso sem endereço ou
dados vendor. A observação planejada de três minutos não foi concluída; o
último resumo do processo estava em 66.270 ms. A existência do processo foi
verificada fora do sandbox, além da evidência independente do log e do kernel.

O código guest salvo é idêntico ao das falhas anteriores. O módulo SPIR-V do
backup também é **idêntico byte a byte ao controle novo sem captura**:
54.636 palavras, SHA-256
`77679e63d590bd12348b91ad3edd7418dc4ea085c121fb42f95fac9bd177b894`.
Todos os 585 registros do cache tiveram seus checksums verificados; esse
módulo passou novamente no validador para `vulkan1.2`. Logo, a captura e a
integração recente do backend hardware não são condições necessárias para
reproduzir a falha. Isso não identifica a instrução exata nem inocenta os
demais caminhos comuns às duas versões.

Os artefatos estão em `_Build/rt-integration-20261010/resume-baseline/`:
manifesto com hashes/ambiente/resultado, logs, journal do kernel, HangTrace,
cache preservado, shader original e módulo extraído. O extrator reproduzível
está em `_Build/rt-integration-20261010/extract-cache-shader.py`.

### Hipótese delimitada: lista sem marcador residente

O log do backup registra oito páginas ausentes do cache GPU, começando em
`0x403b28000`, em torno de 64 segundos de execução. Essa primeira página
contém a raiz `0x403b28700` derivada do descritor e nó da captura anterior:
base `0x403b28600`, nó `0x25`, índice 4, deslocamento `4 * 64`. Os pedidos
seguintes abrangem outras páginas da mesma região. Não há registro dos
pedidos que o dispatch travado possa ter produzido, porque seu readback
depende da conclusão GPU.

Em `0x50c..0x5a8`, o shader percorre pares carregados por `S_BUFFER_LOAD` e
termina quando `s35` é negativo. O código atual retorna zero para leituras
BDA sem página e para acessos além do limite do descritor. Zero não satisfaz
esse marcador. `FaultManager::ProcessFaultBuffer` só materializa as páginas
pedidas depois da conclusão dos comandos que produzem o readback. Isso
define uma possibilidade concreta de falha de terminação, mas ainda não
prova que esse laço recebeu dados ausentes no dispatch que travou. Limite
incorreto de descritor e outras causas continuam abertos.

`--traversal-lists-only` reproduz a leitura escalar de pares por um descritor
selecionado na GPU, com o marcador negativo na terceira entrada. Os três
casos passaram na Radeon, com validação Vulkan `full` e zero erros/avisos:

| Dados/descritor | Entradas lidas | Marcador encontrado |
|---|---:|---|
| Página residente, limite completo | 3 | Sim |
| Página da lista ausente | 32 | Não; leituras retornam zero |
| Página residente, limite anterior ao marcador | 32 | Não; leituras retornam zero |

O contador de 32 entradas pertence exclusivamente à fixture e evita travar a
GPU durante a reprodução. Não usa `KYTY_LOOP_GUARD`, não altera o emulador e
não é uma correção. Os dados do teste são sintéticos; a passagem caracteriza
o risco de perder o marcador, sem demonstrar qual desses casos ocorre no
Astro. Build concluído com código 0; log dos testes em
`_Build/rt-integration-20261010/traversal-lists-full.log`.

O próximo diagnóstico precisa registrar PC, descritor, endereço calculado,
limite e presença da página nas leituras `0x4fc`/`0x5a0` do shader real, com
readback concluído. Isso distinguirá página ausente de limite incorreto e
permitirá verificar a hipótese antes de mudar a política de residência.
Se as leituras estiverem corretas, esta hipótese deve ser rejeitada e a
investigação voltar ao restante do CFG/estado das lanes. Não cabe aumentar
o orçamento global de laços nem habilitar reconvergência como correção
presumida.

Não foi introduzido um retorno artificial de hit/miss nem reativado o
limitador no jogo. O bloqueio da integração hardware permanece: faltam
dados coerentes da travessia real e uma execução estável para comparar.
Nessa comparação, as alterações de código foram apenas nos testes. O binário
instalado foi preservado; o seletor local `KYTY_ASTRO_EMULATOR` permitiu
executar o backup sem substituí-lo. O launcher gerado foi restaurado para o
executável instalado após a comparação. Nenhum commit foi feito.

## Diagnóstico escalar: validação e teste no Astro

Foi acrescentado um probe opt-in selecionado por
`KYTY_SCALAR_READ_PROBE_SHADER` e `KYTY_SCALAR_READ_PROBE_PCS`, com alvo previsto
`7d6ab87e6c984bb7` e PCs `4fc,5a0`. Ele registra a primeira leitura indireta
inválida: hash/PC, quatro palavras do descritor, endereço guest, tamanho,
offset, ponteiro BDA e motivo (limite, endereço fora da janela ou página ausente).
O registro de trap passou de 32 para 80 bytes dentro da folga existente de
256 bytes, preservando o prefixo e o offset do bitmap de escritas BDA.
As opções e o tamanho do registro participam do fingerprint do cache.

Este é um diagnóstico que altera a execução: ao detectar a falha, uma flag
local é marcada e um voto de subgroup força uma saída já existente dos laços.
O host salva o registro após a conclusão GPU e encerra intencionalmente o emulador. Não
constitui correção, fallback de RT, medição de FPS ou prova de imagem correta.
O emissor recusa shaders com barreira de workgroup, dispatcher e waves
virtualizadas. Laços sem uma saída condicional reconhecida não têm garantia
de recuperação. O limitador global de laços permanece desligado.

O readback reutiliza as dependências e a conclusão diferida do FaultManager;
o teste usa barreiras compute-write → transfer-read → host-read, espera de
conclusão e invalidação da memória mapeada. A referência consultada via
Context7 foi [Khronos Synchronization Examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples).

O teste foi executado antes da implementação e falhou pela ausência do registro
esperado (`red-test.log`), demonstrando que não passava sem o probe. A matriz
inclui página ausente, limite curto, leitura saudável e seletores de PC/hash
que não correspondem. O build inicial de `shader_recompiler_compute_tests` e
`kyty_emulator` terminou com código 0 (`green-build.log`). O teste CPU
`--program-cache-only` passou: 51 opções no fingerprint, chaves de origem e
153 truncamentos de arquivo (`program-cache.log`). `git diff --check` passou.
Depois de fechar a outra instância, sete casos passaram na Radeon com
validação `full` (zero erros/avisos) e `sync` (zero erros; um aviso
`VALIDATION-SETTINGS` sobre parsing de SPIR-V com validação de shader
desabilitada nesse modo). No primeiro caso, o teste esperava o endereço da
primeira palavra do par, mas só consome a segunda (marcador). O debugger
confirmou o offset 4 correto; a expectativa foi ajustada para a palavra viva.

A tentativa real `_Build/astro-rt-run/runs/20261010-110009` revelou uma lacuna
da fixture: o `OpReturn` inicial do probe estava dentro do continue do laço
do Astro (PC `0x5a0`), violando a pós-dominância estrutural da aresta de volta.
O validador interrompeu a pré-compilação, com código 65, antes da captura;
nenhum timeout/reset GPU foi registrado nesse intervalo. O módulo inválido
foi extraído do cache e preservado em `scalar-read-probe/invalid-shader/`:
55.778 palavras, SHA-256
`4973a9e43e2e08251ae9633eebe44312fbe2cbf6f4684f3fbce7e70aa2934a92`,
erro `SPV_ERROR_INVALID_CFG`. O retorno direto foi substituído pela flag e
pelas saídas existentes, mantendo as arestas do CFG. A fixture agora cobre
também leituras no continue. O build corrigido terminou com código 0
(`structured-build.log`). Os dez casos passaram na Radeon com validação
`full` (zero erros e avisos) e `sync` (zero erros; o mesmo aviso de configuração
da camada). O teste CPU de cache passou novamente com 51 opções e 153
truncamentos (`structured-program-cache.log`).

A execução real corrigida foi `runs/20261010-111057`, PID 37535, sem patch,
captura BVH, backend hardware ou limitador. Os 305 shaders passaram na
pré-compilação, sem falhas. O usuário confirmou a mesma cena de gameplay e
imagem normal às 11:12:18 -03:00. A execução terminou com código 0; o último
resumo do HangTrace registra 1.125.060 ms desde o início. O diretório do probe
ficou vazio e o journal do kernel não registrou novo timeout/reset/fault de GPU
desde o início desse teste. A causa da saída normal não foi identificada.
A confirmação visual é do início da observação; não houve comparação de FPS.

O cache dessa execução foi preservado e todos os seus 613 registros tiveram
os checksums conferidos. O módulo real instrumentado contém 56.175 palavras,
dez laços e 48 shuffles; passou no SPIRV-Tools para `vulkan1.2`, SHA-256
`cadd1b5407920b791a68c81831fa0b1e8d41e9c4dccce3a837a092c7006a7e79`.
Logs, cache, módulo extraído e manifesto estão em
`_Build/rt-integration-20261010/scalar-read-probe/`, nos subdiretórios
`game-run/` e `validated-shader/`. O binário instalado foi preservado, e o
launcher gerado voltou a apontar para ele após o teste.

O relatório complementar [`ASTRO-RT-HANG-2026-10-10.md`](ASTRO-RT-HANG-2026-10-10.md)
detalha a lista e a correlação com as páginas ausentes. Seu mecanismo proposto
de saída por falta de residência é uma hipótese de correção, ainda não aplicada
ao caminho normal. `reason=3` confirmará uma leitura sem página no ponto
capturado; `reason=1` confirmará o resultado do teste de limite, não provará
sozinho que o tamanho do descritor está errado. Confirmar a causa do timeout
exigirá ainda executar e comparar o caminho corrigido.

Ferramentas disponíveis verificadas: RenderDoc, perf, radeontop, GDB e
vulkaninfo. A biblioteca SPIRV-Tools do build foi usada para validar os
módulos. UMR não está instalado. A
[documentação oficial de hang debugging do RADV](https://docs.mesa3d.org/drivers/amd/hang-debugging.html)
descreve `RADV_DEBUG=hang`, PCs de waves, registradores e relatórios de comandos;
os dados completos via UMR dependem de acesso às interfaces de diagnóstico
do kernel. `RADV_DEBUG=hang` também acrescenta sincronização, portanto sua
execução não equivale a uma medição de desempenho normal. Nesta tentativa,
a validação resolveu a incerteza estrutural sem alterar permissões do kernel.

### Amostragem do dispatch com GDB

Depois da observação inicial, o GDB foi conectado ao processo para amostrar
`RenderExecutor::DispatchDirect`. O executável de diagnóstico estava sem
símbolos; foi ligada uma cópia com os símbolos dos mesmos objetos já
compilados, sem recompilar ou trocar o binário em execução. Endereços e
conteúdo de `.text`, `.rodata`, `.data` e tamanho/endereço de `.bss` foram
conferidos (`symbol-equivalence.json`). As faltas de memória esperadas do
write watch foram encaminhadas ao processo, sem interromper a amostragem.

Uma janela de dez segundos registrou **1.084 dispatches diretos e 14 hashes**,
mas nenhum era `7d6ab87e6c984bb7` (`dispatch-probe-3.log`,
`dispatch-sample.json`). O prefixo `SELECTED DISPATCH` desse log identifica
todos os callbacks do breakpoint, não apenas o hash alvo; o inventário JSON
foi calculado pelos hashes efetivamente impressos. As duas tentativas
anteriores não são evidência de execução do alvo: houve troca automática do
arquivo de símbolos na primeira e uma variável otimizada na segunda.
Uma tentativa posterior de inventário de BVH não conseguiu anexar ao
processo, que já havia terminado; não produziu dados.

Essa amostra registra chamadas no host, não execução de instruções na GPU,
e não cobre dispatches indiretos ou todo o período de jogo. O debugger também
altera o tempo de execução. A ausência do alvo apenas nessa janela limita a
interpretação do diretório vazio: **a causa do timeout não foi confirmada nem
corrigida por este teste**. A próxima reprodução deve incluir o trecho em
que o shader suspeito é enviado, conferindo se a falha ocorre na entrada da
fase ou durante gameplay. RenderDoc está instalado, mas não estava carregado
nesse processo; uma captura exigiria relançar o jogo pela ferramenta.
`RADV_DEBUG=hang` é uma alternativa para obter comandos e shader nativo no
timeout; UMR não foi instalado e as permissões do kernel não foram alteradas.

## Relação com a pesquisa existente

### Referência Khronos: Vulkan Ray Tracing, janeiro de 2021

O PDF local [`Vulkan-Ray-Tracing-Update-Jan-21.pdf`](Vulkan-Ray-Tracing-Update-Jan-21.pdf),
de Daniel Koch/Khronos, foi consultado nesta retomada. As páginas 4–5 explicam
as estruturas opacas construídas pelo driver a partir de geometria e instâncias;
as páginas 6–8 distinguem pipelines RT de consultas de raios dentro de shaders
existentes. Isso sustenta o caminho já usado no protótipo: BLAS/TLAS e
`VK_KHR_ray_query` em compute. Não fornece uma conversão direta da BVH PS5.

A documentação atual foi conferida via Context7 (`/khronosgroup/vulkan-docs`):
[estruturas de aceleração opacas](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/resources.adoc),
[geometria e instâncias](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/accelstructures.adoc)
e [interseções candidatas/confirmadas](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/raytraversal.adoc).
A conversão precisa preservar geometria, transformações, identidade das primitivas
e regras de aceitação do shader guest. A consulta opaca do protótipo não demonstra
essa equivalência para o jogo. Bibliotecas de pipelines (página 9) e construção
no host (página 10) são mecanismos diferentes; não resolvem automaticamente o
travamento da travessia atual, nem justificam ganhos de desempenho presumidos.

O material ajuda a orientar a integração hardware, mas não contém diagnóstico
do timeout do Astro. A investigação da leitura escalar continua necessária.

`docs/perf-research/raytracing-plan.md`, escrito para Astro Bot (PPSA21564),
separa corretamente a interseção de um nó da travessia implementada pelo
shader. O jogo aberto nesta sessão é Astro's Playroom (PPSA01325), confirmado
por `sce_sys/param.json`; endereços e patches do plano não são intercambiáveis.

Parte das etapas propostas já existe no código atual: decode BVH32/BVH64/A16,
IR BVH, função compartilhada de interseção, mapeamento de nós e testes GPU.
Os shaders desta captura também emitiram SPIR-V estruturado. As conclusões
do documento sobre versões antigas e compiles com dispatcher não devem ser
tratadas como diagnóstico automático do build atual.

A seção de hardware RT fornece o trabalho restante: espelho da geometria
guest em BLAS/TLAS, tabela de identificação de primitivas/instâncias,
invalidação quando a geometria muda e reconhecimento da travessia completa,
incluindo sua política de aceitação. O backend e o replay de folhas validam
partes dessa proposta, mas ainda não a executam nos shaders de um jogo.

`roadmap.md` e `raw-findings.md` também apontam custos de readback e
materialização por draw. Esses itens devem ser confrontados com o código e
os perfis atuais: vários já foram alterados nas otimizações desta sessão.
O ganho de FPS depende do gargalo medido, além do custo de construir os AS;
os números antigos da pesquisa não são uma previsão para este teste.

## Captura real e limite zero no cabeçalho

O [diagnóstico escalar](ASTRO-SCALAR-READ-PROBE-2026-10-10.md) reproduziu
uma leitura inválida em `0x4fc` do shader `7d6ab87e6c984bb7`: descritor da
lista em `0x403aac000`, `NumRecords=0`, acesso a `0x403aac004`. A execução
normal do probe encerra intencionalmente com código 65. O modo explícito
`KYTY_SCALAR_READ_PROBE_CONTINUE=1` salva o trap e a lista de páginas do
mesmo lote, permitindo que o caminho existente materialize essas páginas.
Ele preserva a coleta, mas deixa incompleto o dispatch com falha.

Duas execuções com esse modo reproduziram o mesmo trap. Ambas listaram
`0x403aac000`, página do cabeçalho BLAS+88, entre as páginas ausentes.
Isso apoia investigar a residência do cabeçalho; não prova sozinho a
origem do zero nem constitui uma correção do caminho normal.

A captura `bvh-20261010-123424-QLEDPK` contém 64 caixas sem página. O
auditor hardware retornou 77, com zero consultas e 64 registros rejeitados
pelo guard. A captura seguinte, `bvh-20261010-124155-7UH4jD`, contém 23
caixas residentes, mas nenhum triângulo. **Nenhuma dessas amostras permite
afirmar equivalência com RT por hardware.** Seus manifestos, registros e
relatórios estão em `scalar-read-probe/dispatch-observation/` sob a pasta
de artefatos `_Build/rt-integration-20261010/`.

A leitura posterior do backing CPU da BLAS contém `NumRecords=116`,
preservada com timestamp e SHA-256. Sua proveniência é diferente da captura
GPU: não demonstra o valor no instante da leitura inválida. O executável
normal instalado permanece preservado; os testes usam uma cópia diagnóstica.

## Hardware validado com folhas reais do Astro

O filtro opcional `KYTY_BVH_CAPTURE_PC=1312` seleciona a instrução `0x520`.
O PC é decimal e zero mantém todas as instruções. Ele ocupa um campo
reservado do cabeçalho de captura v2 e filtra antes da reserva de um slot;
o resultado da interseção por software permanece o mesmo. O teste GPU do
filtro passou após reproduzir a falha sem a implementação, incluindo seleção
de ambas as instruções da fixture e de um PC inexistente. A suíte também
cobre o encaminhamento desse campo pelo renderer direto/indireto.

Na execução `runs/20261010-125559`, sem patch, foram coletados 64 registros
residentes de `0x520`, com os quatro tipos de triângulo e 12 combinações
distintas de descritor/nó. Não houve trap escalar registrado até a coleta.
A captura é parcial, com capacidade 64 e máscara 4095, SHA-256
`f66af13438bd188980c021a40695ed836996d86b59df583f9dc9e4dc2ea3c3f4`.

Após encerrar o jogo, `--bvh-capture-replay` executou **64 consultas hardware**,
sem registros ignorados, com zero divergências de acerto, distância ou
baricêntricas e zero erros/avisos de validação Vulkan `full`. O processo
retornou 0. Valem as tolerâncias `2e-5` para distância relativa e `1e-4`
para baricêntricas. Manifesto, captura, módulo SPIR-V validado, CSV e log
estão em `scalar-read-probe/dispatch-observation/bvh-20261010-125559-v7Dh4h/`
sob `_Build/rt-integration-20261010/`.

**O hardware agora foi comparado com folhas e raios reais do jogo.** Esse
auditor continua separado da execução do Astro, que usa travessia por
software. Ainda faltam snapshot coerente da árvore completa, instâncias
e política de aceitação para substituí-la. O teste não mede desempenho.

## BLAS completa e política do Astro — captura GPU de 13:52

Em `astro-full-scene/bvh-20261010-135259-GAUmwo/`, sob
`_Build/rt-integration-20261010/`, a captura de `0x520` salvou 64 registros
residentes e 64 sidecars completos. Cada sidecar contém os 928 bytes da
BLAS (incluindo a lista final) e os 160 bytes da instância, lidos pela GPU
através da tabela de páginas no mesmo dispatch. A fixture atravessa páginas
com backing não contíguo e rejeita explicitamente uma página ausente.

A BLAS tem raiz 37 e 12 folhas na ordem:
`72,64,74,73,80,83,65,66,88,67,81,75`. Os IDs são `0x40000000` até
`0x4000000a`, e `0xc000000b` na última entrada. O bit de término do ID é
preservado no resultado; o teste desse triângulo ocorre antes de sair da lista.

SHA-256 do arquivo principal:
`eeb919c512ab132bdfc7a2d9e63d6fee43b416776f125cfd76fa98aac6a214cd`.
SHA-256 do sidecar:
`d9c080b1f08ac896003eb17a13424c051328225d558d371a6c638c19891fe6b9`.

O novo seletor `--astro-scene-replay <principal> <sidecar> <csv>` valida o
pareamento (shader, tick, dimensões e contexto) antes de importar uma cena.
Seu oráculo executa o opcode BVH de produção na GPU, incluindo a caixa raiz,
e aplica a ordem da lista e as regras de face/extent do shader. A comparação
com a AS nativa inteira na RX 9070 XT retornou 0: **64 raios, 25 acertos e 39
raios sem acerto, zero divergências de acerto, nó, ID ou distância**.
Tolerância relativa de distância `2e-5`; maior diferença absoluta observada
`3,9e-6`. CSV: `astro-full-scene/full-blas-parity.csv`; log original:
`/tmp/astro-full-blas-replay.log`. A validação foi repetida com a variável
correta do harness, `KYTY_TEST_VULKAN_VALIDATION=full`: zero erros/avisos,
retorno 0, log `/tmp/astro-full-blas-validation.log` e CSV
`astro-full-scene/full-blas-parity-validated.csv`. A primeira execução usou
`KYTY_VULKAN_VALIDATION`, que não ativa as camadas nesse harness.

Isso valida a BLAS e a política para esses raios, ainda em replay offline.
O shader ao vivo continua por software. O snapshot histórico só pode alimentar
uma substituição futura se os bytes atuais forem novamente conferidos na GPU.
Empates, bordas e cobertura de outras geometrias precisam de fallback explícito;
nenhum ganho de FPS foi medido nesta etapa.

## Substituição experimental no shader ao vivo

O hook `AstroTrace` reconhece apenas o shader `7d6ab87e6c984bb7`, compute,
PC `0x4a0`, wave32/wave64. O código conserva a TLAS do jogo e a transformação
atual da instância; os raios chegam à AS nativa no espaço local da BLAS.
A ligação é privada ao renderer, por endereço da AS, sem acrescentar bindings
ao layout dos shaders do jogo. A cena permanece viva até a conclusão GPU.

O caminho atual admite uma BLAS capturada de até 1024 bytes, uma caixa raiz e
uma lista ordenada com até 64 folhas. Antes de usar o hardware, a GPU compara
descritor, raiz e todos os bytes atuais com o snapshot. Geometria alterada,
página ausente, outra BLAS, raio sem acerto, borda próxima ou regra não coberta
mantêm a travessia original por software. A opção fica desligada por padrão;
exige `KYTY_HW_RT_BACKEND=1`, `KYTY_HW_RT_ASTRO=1` e os dois arquivos
`KYTY_HW_RT_ASTRO_NODES`/`KYTY_HW_RT_ASTRO_SCENES`.

Os candidatos não opacos usam a aritmética de triângulo do opcode de produção
para preservar a regra de face, o extent e os IDs do jogo. Candidatos nativos
não são confirmados: um empate é resolvido pela última entrada da lista,
incluindo o ID com bit de término. Lanes concluídas pelo hardware saem da
travessia BLAS; as demais continuam por software, e o EXEC da instância é
restaurado pelo shader original em `0x9c0`.

`--astro-hook-replay <principal> <sidecar>` verifica os registros vivos de
saída, a pilha e o EXEC com raios capturados, lista alterada, página ausente,
cena indisponível e doze triângulos coincidentes. A primeira execução wave32
passou sob validação `full` (zero erros/avisos) e `sync` (zero erros; um aviso
de configuração por validação de shaders desligada nesse modo).

A execução de gameplay `runs/20261010-142203` abriu sem patch e a imagem foi
confirmada normal pelo usuário. O binário SHA-256
`45005797aa1312ced1a68cc3055e149df1c5f16ffbf46593384b0d3031d3bbc8`
vinculou as 12 folhas, mas reportou **zero raios nativos e zero fallbacks**.
Isso não comprova execução do hook. A investigação do journal real encontrou
wave64, enquanto o primeiro guard do hook admitia somente wave32. A fixture
wave64 reproduziu `AstroTrace count=0, expected=1` antes da correção.
A captura de tela do usuário às 14:27 mostra 3,5 FPS com diagnóstico/validação;
esse resultado não é uma comparação de desempenho. A nova validação wave64
e o teste ao vivo devem confirmar contadores positivos antes de afirmar RT
por hardware no jogo. Ainda não há substituição geral de todas as BVHs/TLAS
nem ganho de FPS comprovado.

A correção passou em `--astro-hook-replay` para **wave32 e wave64**: 25
acertos reais por modo, zero acertos nativos nos três casos de fallback e
64 acertos com o último ID no empate. Validação `full`: zero erros/avisos;
`sync`: zero erros e o mesmo aviso de configuração do harness. A suíte CTest
pertinente passou **7/7** (captura GPU/reader/replay, conversão BVH, Astro,
backend RT ativo e desligado). Logs:
`/tmp/astro-wave64-green-full.log`, `/tmp/astro-wave64-green-sync.log`,
`/tmp/astro-integration-final-ctest.log`; cópias em
`_Build/rt-integration-20261010/astro-native-wave64-run/`.

O build corrigido, SHA-256
`9a4d164292def729dab20356ce60de5dcc33631ff930e58776110e69386e88bf`,
foi reaberto sem patch em `runs/20261010-143051`. O usuário confirmou imagem
normal na floresta e depois de caminhar pelo trecho. O journal registrou a
compilação da pipeline compute do shader alvo em `t_ms=185157`; a cena nativa
foi vinculada. A primeira leitura GPU retornou contadores zerados. A aceitação
de gameplay nessa primeira leitura ainda dependia de contadores positivos e estabilidade; pré-compilar
um shader e vincular a cena não comprovam o uso nativo dos raios.

## Resultado do teste ao vivo com wave64

Na mesma execução, leituras posteriores concluídas pela GPU reportaram:

| Resultados nativos | Fallbacks por software |
| ---: | ---: |
| 10.402.461 | 237.533.458 |
| 15.121.147 | 315.361.701 |
| 19.880.685 | 393.759.997 |

São contadores cumulativos de entradas no helper BLAS, **não raios únicos**
nem cobertura percentual do RT inteiro do jogo. Um raio pode entrar no helper
várias vezes; consultas nativas sem acerto também continuam por software.
O crescimento de resultados usados pelo shader confirma execução nativa no
gameplay, além da simples disponibilidade das extensões Vulkan.

Às 14:37:08, a instância continuava ativa, mais de três minutos após a ativação
da pipeline alvo. Não apareceram VUIDs, erros de validação ou perda do device
nos dois logs da execução. Depois de caminhar pelo trecho, o usuário confirmou
que a imagem continuava normal, sem novas manchas, partes pretas ou travamentos.
Evidência: `astro-native-wave64-run/runtime-verification.json`, manifesto,
logs e SHA-256 do executável lido diretamente de `/proc/137133/exe`.

A regressão wave64 foi registrada no CTest como `astro_hardware_rt_hook`;
a suíte final pertinente passou **8/8**. O replay do hook continua disponível
para validação GPU dos registros de saída com os arquivos reais capturados.

O protótipo está integrado e foi exercitado no Astro, mas ainda cobre somente
a BLAS importada. **A maior parte das entradas continua por software**.
Para RT geral ainda faltam importação coerente dinâmica de outras BLAS,
árvores/listas maiores com ordem de empate dependente do raio, outros shaders
e substituição da TLAS. Ganho de FPS permanece **não medido**; a sessão usa
validação Vulkan e diagnóstico escalar, inclusive o modo de continuação.
Não houve trap escalar registrado nesta execução. A opção continua desligada
por padrão e esse build não deve ser tratado como benchmark de desempenho.

## Encerramento e retomada

Código e testes commitados em `90200ebf` na branch `guest-sync-release-mem`.
A execução terminou regularmente: retorno 0, `Window 5 closed` e `Event: quit`
no log, sem perda do device ou erro de validação registrado. A sessão durou
cerca de 7 minutos; a pipeline alvo foi ativada após cerca de 3 minutos.
Últimos contadores: **24.638.470 resultados nativos e 472.193.263 fallbacks**.
A imagem foi confirmada normal pelo usuário depois da ativação do hardware.

Trabalho interrompido a pedido do usuário após documentar e commitar.
Na retomada, priorizar importação coerente dinâmica de outras BVHs e redução
do custo de conferir os bytes atuais. Depois ampliar árvores/listas e regras
de outros shaders, tratar TLAS nativa e medir FPS com diagnóstico desligado.
O estado atual é um protótipo limitado, sem ganho de desempenho comprovado.
Launcher preservado:
`_Build/rt-integration-20261010/astro-native-wave64-run/start.sh`.
Não houve alteração do executável normal instalado nem envio ao remoto.

## Caminho para RT completo

O teste confirmou a integração de uma BLAS real ao shader. Ainda dependemos de
arquivos históricos, uma árvore pequena e um shader específico. Para ampliar
a cobertura do Astro, seguir esta ordem; suporte geral a outros jogos exige
validar também seus formatos e shaders.

| Etapa | Trabalho necessário | Arquivos principais | Critério de validação |
| --- | --- | --- | --- |
| 1. Importação dinâmica coerente | Descobrir as BLAS usadas pelo dispatch e obter seus bytes atuais pela GPU, sem exigir dumps externos. Tratar páginas ausentes antes de aceitar a cena e não usar backing CPU posterior como prova. | `renderer/cache/faultManager.cpp`, `renderer/renderCompute.cpp`, `renderer/rt/astroBvh.cpp` | Mudança de cena, streaming e páginas ausentes: snapshot correto ou fallback explícito, sem leitura inválida ou resultado inventado. |
| 2. Árvores maiores e cache | Remover o limite do protótipo de uma caixa/lista e 1024 bytes; converter árvores completas e preservar nó/ID de cada folha. Manter AS por geometria e versão, com reconstrução ou atualização quando necessário. | `renderer/rt/guestBvh.cpp`, `renderer/rt/hardwareRt.cpp`, `shader/recompiler/AstroNativeBinding.h` | Comparação GPU software/hardware para várias BLAS; alterações de geometria invalidam a AS e recursos antigos sobrevivem até a conclusão GPU. |
| 3. Regras e shaders | Cobrir a ordem de travessia de várias caixas/listas, empates, faces, distância e saídas vivas. Ampliar o reconhecimento apenas após verificar cada shader do jogo; casos ambíguos podem continuar em software até serem comprovados. | `shader/recompiler/frontend/translate/Dispatch.cpp`, `shader/recompiler/backend/spirv/spirvEmitterAstroRt.inc` | Mesmo acerto, nó, ID, instância e distância que o shader original; EXEC e pilhas corretos em waves mistas. |
| 4. TLAS e instâncias nativas | Importar a hierarquia de instâncias e suas transformações atuais; associar resultados nativos aos ponteiros/IDs esperados pelo jogo. Hoje essa travessia permanece no shader original. | `renderer/rt/astroBvh.cpp`, `renderer/rt/hardwareRt.cpp`, frontend e emitter | Instâncias múltiplas, movimento, escala não uniforme e reflexão: resultados equivalentes e AS sincronizadas. |
| 5. Desempenho | Reduzir a conferência de memória feita hoje por entrada no helper, reaproveitar cenas válidas e diminuir trabalho duplicado. A reutilização deve preservar a prova de que os bytes usados continuam atuais. | `renderer/rt/hardwareRt.cpp`, `renderer/renderCompute.cpp`, `spirvEmitterAstroRt.inc` | Perfil mostra redução de custo; comparação na mesma cena, sem compilação concorrente, validação, captura ou probe escalar. |
| 6. Aceitação do caminho normal | Testar sem patch e sem instrumentação que modifica a saída, incluindo carregamento, caminhada, transições e sessões prolongadas. Medir cobertura nativa e falhas, além da imagem. | Testes BVH/RT e `docs/ASTRO-RT-HANG-2026-10-10.md` | Imagem correta, ausência de travamentos/perda do device, regras suportadas documentadas e ganho de FPS medido antes de ativar por padrão. |

Os caminhos da tabela são relativos a `src/graphics/`. Começar pelas etapas
1 e 2 amplia a cobertura que hoje deixa a maioria das entradas em software.
A TLAS nativa amplia a substituição completa; a integração BLAS híbrida já
pode continuar sendo testada enquanto essa etapa é preparada.

Não há porcentagem confiável de conclusão do RT completo: os contadores do
protótipo contam chamadas ao helper, não todas as operações RT do jogo.
O trabalho permanece interrompido a pedido do usuário; este roteiro registra
a retomada, sem iniciar nova implementação.
