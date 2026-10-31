# OpenIris UDP Bridge — Projeto

## 1. Visão geral

Desenvolver uma solução de baixa latência para **três câmeras ESP32 com firmware baseado no OpenIris**, conectadas por Wi-Fi 2,4 GHz. Cada câmera capturará imagens do olho e transmitirá frames JPEG usando **UDP** para um aplicativo local no PC. O aplicativo será **headless (sem interface gráfica)**, executado pelo terminal, reconstruirá os frames e disponibilizará **três streams HTTP MJPEG locais**, inicialmente para consumo pelo **Baballonia**, que continuará responsável pelo processamento e rastreamento ocular.

**Fluxo desejado:**

```text
ESP32 1 -- UDP/JPEG --\
ESP32 2 -- UDP/JPEG ----> PC: OpenIris UDP Bridge ---> HTTP MJPEG localhost ---> Baballonia ---> tracking
ESP32 3 -- UDP/JPEG --/
```

**Objetivo:** eliminar a transmissão HTTP MJPEG sobre o trecho Wi-Fi (potencial fonte de bloqueios e filas), mantendo a compatibilidade com o formato de vídeo já conhecido pelo software receptor, sem precisar reimplementar o algoritmo de tracking.

> **Limitação importante:** UDP não torna a rede imune à interferência nem garante streaming sem travamentos. O objetivo é evitar *travamentos prolongados e atraso acumulado*: pacotes ou frames incompletos devem ser descartados, e o sistema deve priorizar sempre o frame completo mais recente.

## 2. Situação atual e motivação

- Três câmeras ESP32 compartilham uma rede Wi-Fi de 2,4 GHz.
- O OpenIris original oferece streaming de imagens MJPEG via HTTP; é possível encontrar quedas de qualidade, congelamentos e latência com fluxos simultâneos.
- O transporte atual utiliza HTTP sobre TCP, que preserva a ordem dos bytes e pode acumular atrasos quando há perdas/retransmissões.
- A proposta **não** é trocar simplesmente TCP por UDP: é criar um protocolo de frames com limites explícitos de buffer, identificação de frames e descarte de dados atrasados.
- O projeto original OpenIris está arquivado e aponta para um novo desenvolvimento; avaliar a base de firmware escolhida antes de integrar mudanças.

## 3. Escopo

### Incluído no MVP

1. Recriar/adaptar o firmware ESP32 como **projeto PlatformIO compilável**, com captura JPEG, envio UDP, descoberta do PC e perfis de hardware adequados.
2. Desenvolver aplicativo local que **descubra automaticamente as câmeras na mesma rede** e receba as três transmissões simultaneamente, sem exigir cadastro manual de endereços IP.
3. Reconstruir cada imagem por câmera, validar seus pacotes e descartar frames incompletos ou ultrapassados.
4. Disponibilizar um endpoint HTTP MJPEG separado por câmera, vinculado por padrão a `127.0.0.1`.
5. Exibir no terminal, sem interface gráfica, métricas por câmera: FPS recebido, FPS completo, perdas, frames descartados, bytes/s, idade do último frame e estado da conexão.
6. Testar integração dos URLs locais com o Baballonia e ajustar o formato HTTP MJPEG conforme necessário.
7. Implementar recuperação automática após quedas de Wi-Fi, reinicializações ou interrupções do stream.
8. Fazer pareamento e identificação persistente das câmeras, com associação de cada dispositivo a um papel configurável (olho esquerdo, olho direito ou face).

### Fora do escopo inicial

- Criar um novo algoritmo de rastreamento ocular no aplicativo intermediário.
- Criar interface gráfica (GUI), dashboard web ou janela de pré-visualização de vídeo no MVP.
- Garantir FPS fixo ou zero perda de frames em qualquer ambiente Wi-Fi.
- Alterar internamente o Baballonia sem necessidade comprovada.
- Usar retransmissão confiável de frames antigos como comportamento padrão.

## 4. Arquitetura

### 4.1 Firmware ESP32 (produtor)

- Captura imagens com as configurações disponíveis da câmera.
- Faz compressão JPEG na origem, quando suportada pelo sensor/firmware; caso a origem seja escala de cinza, confirmar suporte ou custo da codificação JPEG.
- Divide JPEGs em datagramas de tamanho seguro, com payload inicial sugerido de **~1.200 bytes** por pacote, para reduzir o risco de fragmentação IP.
- Adiciona cabeçalho binário com versão de protocolo, ID da câmera, ID do frame, índice do pacote, total de pacotes, tamanho dos dados e timestamp de captura.
- Descobre o PC automaticamente por serviço mDNS/DNS-SD (preferencialmente) ou responde a anúncio/sondagem UDP na LAN; o destino pode ser configurado manualmente como fallback.
- Envia pacotes UDP ao IP/porta descobertos do computador; **uma única porta de recepção, como `5000`, pode servir às três câmeras** porque o cabeçalho identifica a origem. Portas distintas são opcionais.
- Limita taxa de frames e qualidade JPEG por configuração.
- Prioriza a captura atual e evita filas ilimitadas.
- Mantém um canal de configuração e diagnóstico (por exemplo, HTTP de controle), se isso for compatível com o firmware escolhido; não precisa manter MJPEG sobre Wi-Fi.

### 4.2 Aplicativo local no PC (ponte)

**Descoberta automática:** serviço mDNS/DNS-SD anunciado pelo PC e descoberto pelas ESP32, e descoberta das câmeras pelo PC (aproveitando os recursos mDNS já existentes no OpenIris, mediante validação dos nomes/tipos de serviço). Como fallback, beacon/sondagem UDP em broadcast na sub-rede, ou cadastro de IP manual. A descoberta deve encontrar câmeras novas, detectar reinícios, lidar com mudanças de IP e informar quando a rede bloqueia multicast/broadcast.

**Camada de entrada UDP:** socket(s) para receber pacotes, identificar a câmera por identificador estável e separar o estado de cada stream.

**Reconstrução:** buffer temporário limitado por câmera; remontagem fora de ordem; detecção de duplicatas, pacotes ausentes e expiração de frames; descarte imediato de JPEG incompleto após timeout.

**Camada de frames:** fila de **um frame completo mais recente** por câmera (política *latest-frame-wins*). Um cliente lento não deve fazer crescer a fila de imagens.

**Servidor de saída HTTP:** disponibilizar `multipart/x-mixed-replace` com `image/jpeg` para que cada entrada possa ser consumida como stream MJPEG local. Cada conexão mantém fila de saída curta e fecha/reconecta clientes travados.

**Interface/monitoramento:** **exclusivamente terminal (CLI/TUI textual simples), sem janela, painel web ou pré-visualização de vídeo**. Exibir câmera detectada, ID persistente, IP atual, papel atribuído, estado e métricas por câmera; registrar eventos de descoberta, conexão, desconexão e reconexão. Atribuições e parâmetros serão definidos inicialmente por arquivo `config.yaml` (ou argumentos CLI), sem menus gráficos. As atribuições sobrevivem a mudanças de IP e reinícios. Logs devem ser legíveis, com opção de saída JSON para automação futura.

### 4.3 Integração com Baballonia (consumidor)

Endpoints **propostos** (não são APIs já verificadas):

```text
http://127.0.0.1:8080/camera/1.mjpeg
http://127.0.0.1:8080/camera/2.mjpeg
http://127.0.0.1:8080/camera/3.mjpeg
```

O aplicativo local hospeda os streams, e **o Baballonia realiza o tracking**. A ponte não precisa interpretar os olhos.

**Ponto de validação obrigatório:** confirmar que a versão em uso do Baballonia aceita URLs MJPEG HTTP locais na configuração de cada câmera e que o formato de multipart, cabeçalhos, resolução, orientação e identificação do dispositivo são compatíveis. Se a interface do Baballonia exigir descoberta específica, API do OpenIris ou outro mecanismo de entrada, implementar um *adaptador de compatibilidade* em vez de presumir que somente três URLs bastam.

O HTTP MJPEG entre dois processos no mesmo PC utiliza a interface de loopback; portanto, o custo de transporte sobre Wi-Fi não se repete nessa etapa. Entretanto, o cliente ainda pode ser lento: filas e timeouts continuam necessários.

## 4.4 Execução headless e observabilidade no terminal

**Requisito de produto:** o programa inicia pelo terminal, descobre automaticamente câmeras sem IP pré-configurado, publica os endpoints locais e imprime estados e métricas. **Não deve abrir nenhuma janela**. O stream MJPEG é servido silenciosamente em background enquanto o processo permanece em execução.

Exemplo ilustrativo de log (endereços e valores fictícios):

```text
$ openiris-udp-bridge --config config.yaml
[12:00:00] INFO  UDP listening on 0.0.0.0:5000 | HTTP on 127.0.0.1:8080
[12:00:01] FOUND camera=cam-a31f ip=192.168.1.101 role=left_eye
[12:00:01] FOUND camera=cam-b42e ip=192.168.1.102 role=right_eye
[12:00:02] FOUND camera=cam-c53d ip=192.168.1.103 role=face
[12:00:05] STATS cam-a31f ONLINE fps=29.8 loss=1.2% bitrate=2.4Mbps last=18ms
[12:00:05] STATS cam-b42e ONLINE fps=29.4 loss=2.0% bitrate=2.3Mbps last=21ms
[12:00:05] STATS cam-c53d ONLINE fps=30.0 loss=0.4% bitrate=2.5Mbps last=15ms
[12:00:06] HTTP  /camera/1.mjpeg  /camera/2.mjpeg  /camera/3.mjpeg
[12:00:12] WARN  cam-b42e OFFLINE (no complete frame for 3s)
[12:00:14] INFO  cam-b42e RECONNECTED ip=192.168.1.102
```

- Logar eventos imediatamente; imprimir resumo de estatísticas a cada **5 segundos** por padrão, configurável para evitar poluição do terminal.
- Estados por câmera: `DISCOVERED`, `CONNECTING`, `STREAMING`, `DEGRADED`, `OFFLINE`.
- Métricas: FPS completo, pacotes perdidos (estimado), frames incompletos/descartados, taxa de recebimento, tempo desde último frame completo, origem IP e número de reconexões.
- Não apresentar “latência de captura” absoluta sem sincronização de relógios ESP32/PC; mostrar idade local do frame e, se houver relógios sincronizados, latência fim a fim estimada.
- Permitir `--log-level`, `--stats-interval`, `--config`; encerrar de forma limpa com `Ctrl+C`.
- Sem configuração inicial dos papéis, listar as câmeras por ID e permitir edição posterior do arquivo de configuração. Os endpoints devem continuar estáveis após mudanças de IP.

Exemplo de configuração inicial (valores ilustrativos):

```yaml
udp:
  listen_host: 0.0.0.0
  port: 5000
http:
  host: 127.0.0.1
  port: 8080
discovery:
  mdns: true
  udp_broadcast_fallback: true
monitoring:
  stats_interval_seconds: 5
  log_level: INFO
cameras:
  cam-a31f: {role: left_eye, endpoint: 1}
  cam-b42e: {role: right_eye, endpoint: 2}
  cam-c53d: {role: face, endpoint: 3}
```

Os IDs do exemplo não são IDs reais. A descoberta automática depende de o firmware ESP32 implementar a descoberta/registro correspondente e de o roteador permitir a comunicação local necessária.

## 5. Descoberta automática, pareamento e reconexão

**Requisito obrigatório:** ao abrir o app do PC, as ESP32 na mesma LAN devem ser encontradas automaticamente, sem digitar seus IPs. O firmware recriado precisa implementar o lado correspondente deste protocolo.

### 5.1 Descoberta recomendada

1. O PC anuncia um serviço de recepção, por exemplo `_openiris-udp._udp.local`, por **mDNS/DNS-SD**, com porta e versão de protocolo. Nome e tipo são **novos e propostos para esta ponte**, não interfaces oficiais do OpenIris.
2. Cada ESP32 procura o serviço, obtém endereço/porta do PC e inicia o registro via mensagem de controle contendo ID estável da câmera, nome, modelo, versão de firmware, capacidades e endereço atual.
3. O PC mantém um inventário de dispositivos vistos, inclusive câmeras temporariamente offline; associa cada ID a **olho esquerdo**, **olho direito** ou **face** por configuração persistida e registra a associação no terminal.
4. O PC confirma o registro e autoriza o início de streaming, informando parâmetros como FPS máximo e porta UDP.
5. Heartbeats e timeout de presença indicam desconexão; após a ESP32 religar ou trocar de IP, o app a reconhece pelo **ID persistente**, nunca apenas pelo IP.
6. Se mDNS não funcionar na rede, fornecer descoberta alternativa por **UDP broadcast** na sub-rede e configuração manual de endereço. Broadcast geralmente não atravessa sub-redes/VLANs e pode ser bloqueado por isolamento de clientes; documentar isso.

O OpenIris original já menciona **mDNS** para descoberta e comunicação com o tracker; reaproveitar essa ideia onde possível. Não presumir que o firmware atual anuncie o novo serviço acima: será necessário adicioná-lo ou adaptá-lo. Para evitar ambiguidades com múltiplas máquinas, possibilitar seleção/pareamento com o PC correto e persistir o identificador do servidor.

### 5.2 Regras de confiabilidade

- Separar tráfego de **controle/descoberta** da transmissão contínua de frames; não bloquear a recepção de vídeo em operações de descoberta.
- Não confiar apenas em mensagens de broadcast para determinar se o streaming está saudável: monitorar o último frame JPEG completo recebido.
- Para impedir múltiplos dispositivos com IDs idênticos, gerar ou derivar um identificador estável único e validar colisões.
- Limitar e validar anúncios/frames de dispositivos desconhecidos, oferecer **pareamento/allowlist** para evitar fontes não autorizadas na rede local.
- Não assumir multicast/mDNS disponível em todas as configurações de roteador; oferecer fallback e mensagens de diagnóstico específicas.
- Persistir a associação entre ID estável e papel; ao restabelecer a conexão, restaurar o endpoint local correspondente, mesmo com IP diferente.

## 6. Protocolo UDP proposto (v1)

Cada datagrama contém `header + payload`. Especificação detalhada a fechar antes da implementação:

| Campo | Objetivo |
|---|---|
| Magic + versão | Identificar pacote e versão |
| Camera ID | Identificar stream |
| Frame ID (uint32) | Detectar quadros novos/antigos |
| Packet index (uint16) | Reordenar segmentos |
| Packet count (uint16) | Saber quando o JPEG está completo |
| Capture timestamp | Medir atraso desde a captura |
| Payload length | Validar tamanho recebido |
| Payload | Bytes do JPEG |

Regras:

- Enviar datagramas abaixo do MTU efetivo, evitando fragmentação IP.
- Limitar quantidade de frames em reconstrução e memória por câmera.
- Validar índices, tamanhos e identificadores antes de alocar memória.
- Aceitar reordenação de pacotes; descartar duplicatas.
- Abandonar frames incompletos depois de timeout configurável.
- Se um frame mais recente estiver disponível, nunca bloquear o stream esperando um frame antigo.
- Considerar CRC/checksum de frame se os testes justificarem o custo.
- Usar controle de versão para permitir evolução do protocolo.

**Observação:** um único datagrama perdido pode invalidar o JPEG inteiro, então testes práticos devem ajustar tamanho de pacote, FPS, compressão e timeout.

## 7. Tecnologias sugeridas

- **Firmware:** projeto **PlatformIO** em C/C++ com `platformio.ini`, preferencialmente usando o framework ESP-IDF (`framework = espidf`) e reaproveitando código compatível do OpenIris. A seleção exata de placa, sensor, pinos e PSRAM é obrigatória.
- **Protótipo do PC:** Python com `asyncio` ou sockets, servidor HTTP assíncrono (`aiohttp` ou equivalente) e saída de métricas apenas no terminal (`logging`; `rich` opcional, sem dependência de GUI).
- **Produção (opcional):** C++/Rust/.NET, caso medições revelem gargalo no protótipo. Não migrar sem necessidade comprovada.
- **OpenCV:** apenas como ferramenta opcional de depuração/visualização; não é necessário decodificar e recodificar JPEG em cada frame se a ponte só repassar os bytes.

## 7.1 Requisito de entrega do firmware: PlatformIO

O firmware ESP32 deve ser **entregue como projeto PlatformIO**, com `platformio.ini` na raiz, código-fonte C/C++, dependências declaradas, arquivos de configuração e documentação de build/flash/monitor serial. A finalidade é permitir abrir o projeto no VS Code com a extensão PlatformIO, compilar e gravar cada ESP32 sem depender de binários precompilados.

Estrutura **proposta** (não corresponde necessariamente à árvore atual dos repositórios OpenIris):

```text
firmware/
├── platformio.ini
├── CMakeLists.txt                 # se usar framework ESP-IDF
├── sdkconfig.defaults           # se necessário, sem segredos
├── boards/                      # perfis de pinagem/sensor/PSRAM
├── include/
├── src/
│   ├── CMakeLists.txt            # se usar framework ESP-IDF
│   ├── main.cpp
│   ├── camera_capture.cpp
│   ├── udp_streamer.cpp
│   ├── discovery.cpp
│   └── wifi_manager.cpp
└── README.md
```

Exemplo **ilustrativo** de configuração; **`esp32-s3-devkitc-1` não deve ser usado sem confirmar a placa e o sensor reais**:

```ini
[env:esp32s3_example]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
monitor_speed = 115200
```

Comandos esperados, após configurar a placa e a porta corretas:

```bash
pio run -e esp32s3_example
pio run -e esp32s3_example -t upload
pio device monitor -b 115200
```

Requisitos adicionais:

- Criar **um ambiente por variante de hardware** efetivamente utilizada; não supor que os três módulos têm a mesma pinagem.
- Documentar sensor, pinagem, clock, flash, PSRAM, qualidade JPEG, FPS, credenciais Wi-Fi (fora do repositório) e parâmetros do protocolo UDP.
- Preservar, se possível, configuração persistente de Wi-Fi, ID estável, diagnóstico serial e recuperação após perda da rede.
- Fornecer instruções de flash, configuração inicial e testes de descoberta automática.
- Evitar incluir senhas de Wi-Fi em `platformio.ini` ou no controle de versão.
- Se portar código do **OpenIris-ESPIDF** para PlatformIO, validar CMake, componentes, `sdkconfig`, versões do ESP-IDF e perfis `boards/`; **não assumir compatibilidade direta sem adaptações**, já que o fluxo documentado do upstream usa ESP-IDF nativo.
- Se a placa escolhida não for suportada pelo PlatformIO/ESP-IDF selecionado, registrar o bloqueio e ajustar o ambiente antes de avançar.

Referências específicas do toolchain:

- PlatformIO + ESP-IDF: https://docs.platformio.org/en/latest/frameworks/espidf.html
- Configuração `platformio.ini`: https://docs.platformio.org/en/latest/projectconf/index.html
- Repositório de origem do novo firmware: https://github.com/EyeTrackVR/OpenIris-ESPIDF

## 8. Metas de desempenho e critérios de aceitação

Metas iniciais sujeitas a medição no hardware real:

1. Receber **3 streams simultâneos** sem bloqueio cruzado entre câmeras.
2. Operar nos FPS suportados pelos dispositivos e rede, buscando **30 FPS estáveis por câmera** como primeiro marco, com testes posteriores a 60 FPS.
3. Evitar crescimento de latência ao longo do tempo: não acumular filas de frames antigos.
4. Recuperar automaticamente o stream após perdas ou reinício de uma ESP32.
5. Manter cada endpoint local acessível de forma independente.
6. Medir e registrar FPS, taxa de perda, frames descartados, atraso estimado e uso de CPU/RAM.
7. Confirmar rastreamento funcional no Baballonia com **três entradas**, se suportadas/configuráveis na versão escolhida.
8. Detectar automaticamente as três câmeras na LAN, lembrar seus papéis e reencontrá-las após reinício ou mudança de IP.

**Não considerar o projeto concluído apenas porque aparecem imagens no navegador:** a validação final exige teste prático com Baballonia, medindo latência perceptível e estabilidade durante sessões prolongadas.

## 9. Testes planejados

| Teste | Procedimento | O que verificar |
|---|---|---|
| Baseline | 1, 2 e 3 câmeras com HTTP MJPEG atual | FPS, congelamentos, tráfego |
| UDP isolado | Uma ESP32 enviando ao PC | Reconstrução e perdas |
| Concorrência | Três ESP32 simultâneas | Ausência de bloqueio cruzado |
| Perdas | Simular descarte/reordenação de datagramas | Timeout e descarte de frame |
| Reconexão | Desligar e religar uma câmera | Recuperação automática |
| Descoberta automática | Abrir o app com três câmeras na LAN, sem cadastrar IPs | Encontrar as três e listar identidade/IP/estado |
| IP alterado | Renovar DHCP/reiniciar roteador | Reconhecer câmera por ID e retomar endpoint sem intervenção |
| mDNS bloqueado | Bloquear multicast ou habilitar isolamento de clientes | Exibir diagnóstico e oferecer descoberta/fallback manual |
| Cliente lento | Consumidor HTTP lendo devagar | Fila limitada; frames recentes |
| Integração | Baballonia consumindo streams locais | Tracking funcional e compatibilidade |
| Longa duração | Executar por horas | Memória estável, sem aumento progressivo da latência |

Também testar o PC conectado ao roteador via **Ethernet**, Wi-Fi em canal menos congestionado com 20 MHz e sinal adequado das três câmeras. UDP não elimina limitações físicas de rádio.

## 10. Plano de implementação

### Fase 1 — Protocolo e prova de conceito
- Selecionar modelo exato de ESP32, sensor e base do firmware; definir ambiente PlatformIO e configurações de placa/PSRAM.
- Documentar formato binário dos datagramas.
- Implementar envio UDP em uma câmera.
- Criar receptor que remonta JPEG e salva/exibe frames.

### Fase 2 — Multiplexação e robustez
- Suportar três câmeras e implementar descoberta automática (mDNS/DNS-SD, fallback UDP broadcast), registro e heartbeats.
- Criar identificação e atribuição persistente via arquivo de configuração e mensagens no terminal para olho esquerdo, olho direito e face.
- Adicionar timeouts, descarte de frames, limites de memória e métricas.
- Testar quedas e interferência.

### Fase 3 — Streaming HTTP local
- Implementar endpoints MJPEG de loopback.
- Impedir que cliente lento bloqueie recepção UDP.
- Validar em navegador ou cliente MJPEG independente.

### Fase 4 — Baballonia
- Confirmar mecanismo suportado de entrada de vídeo.
- Integrar cada stream à câmera correspondente.
- Corrigir cabeçalhos, descoberta ou controles conforme necessário.
- Testar tracking de olhos/face conforme suporte real do Baballonia.

### Fase 5 — Otimização
- Medir latência e gargalos antes de ajustar FPS, compressão, datagramas e buffers.
- Criar configuração persistente, logs no terminal e em arquivo, inicialização automática como processo/serviço e distribuição do app sem GUI.

## 11. Riscos e mitigação

| Risco | Mitigação |
|---|---|
| Interferência no Wi-Fi 2,4 GHz | AP dedicado, bom sinal, canais adequados, controle de bitrate |
| Perda de UDP invalidando JPEGs | Frames pequenos, menor FPS/bitrate, descarte rápido, métricas |
| Baballonia não aceitar os endpoints simples | Adaptador para entrada efetivamente suportada, após validação de código/API |
| Cliente MJPEG lento gerar filas | Buffer de tamanho 1; timeout, descarte e desconexão de cliente lento |
| Firmware não suportar câmera/JPEG como esperado | Validar placa, sensor e pipeline antes de portar |
| Alto uso de CPU no PC | Repassar JPEG sem transcoding e perfilar antes de otimizar |

## 12. Questões em aberto

- Qual é o modelo exato das três placas ESP32 e seus sensores?
- Qual versão/branch do OpenIris será usada?
- Qual versão do Baballonia será usada e como configura fontes de vídeo?
- As três entradas correspondem a olho esquerdo, direito e câmera facial?
- Qual FPS, resolução e tamanho médio dos JPEGs medidos hoje?
- O PC está conectado ao roteador por Ethernet ou Wi-Fi?

## 13. Referências e base para recriar o firmware ESP32

**Código-fonte e documentação para a implementação:**

- **Firmware original OpenIris (código-fonte, arquivado):** https://github.com/EyeTrackVR/OpenIris
- **Documentação oficial do EyeTrackVR (inclui configuração e informações do firmware):** https://docs.eyetrackvr.dev/
- **Novo firmware em ESP-IDF, reescrito em C/C++ (base recomendada para avaliar a alteração UDP):** https://github.com/EyeTrackVR/OpenIris-ESPIDF
- **Releases/firmwares compilados do OpenIris original:** https://github.com/EyeTrackVR/OpenIris/releases
- **Releases do OpenIris-ESPIDF:** https://github.com/EyeTrackVR/OpenIris-ESPIDF/releases
- **Baballonia (código-fonte do programa consumidor):** https://github.com/Project-Babble/Baballonia
- **Documentação do Baballonia no repositório:** https://github.com/Project-Babble/BabbleDocs/blob/main/docs/software/baballonia/baballonia.mdx

**Orientação de implementação:** clonar o repositório do firmware apropriado à placa utilizada, identificar os módulos de captura, Wi-Fi, descoberta mDNS e streaming e **substituir/adicionar a saída UDP mantendo os controles necessários**. No OpenIris-ESPIDF, começar pelo README e pelos diretórios de código e perfis de placas; selecionar o perfil exato antes de compilar e gravar. Não usar binário de placa diferente.

O OpenIris original declara ter mDNS e prevê streaming via sockets em seu TODO; isso **não comprova** que a nova saída UDP já exista. A descoberta automática e o protocolo de transporte especificados aqui serão implementações novas ou adaptações que precisam ser testadas. A interface exata de entrada do Baballonia também precisará ser conferida no código/versão utilizada.

---

**Resumo:** aplicativo **somente terminal, sem GUI**; recriar/adaptar o firmware ESP32 **em projeto PlatformIO** para captura JPEG + **descoberta automática da ponte no PC** + UDP; o aplicativo descobre/pareia automaticamente as três câmeras e publica **UDP → MJPEG local**, priorizando frames recentes, recuperação automática e compatibilidade validada com Baballonia. A expectativa é **reduzir travamentos e latência acumulada**, não prometer ausência absoluta de perdas ou congelamentos.
