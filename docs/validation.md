# Validação e referências

## Escopo validado automaticamente

- Build C++17 com MSVC/Windows x64.
- Build completo do firmware, incluindo bootloader, com ESP-IDF 5.5.5, PlatformIO `espressif32@6.13.0` e Xtensa GCC `14.2.0+20260121`, reaproveitando o compilador instalado neste computador.
- Imagem ESP32-S3 verificada por `esptool image_info`: checksum e hash válidos. `firmware.bin`: 861.344 bytes; RAM estática reportada pelo linker: 43.788 bytes (não inclui alocações em execução, buffers JPEG ou tarefas).
- Configuração gerada conferida: flash DIO de 8 MB, PSRAM octal a 80 MHz, console USB Serial/JTAG e ULP desativado.
- Compilação adicional do caminho POSIX com GCC/Cygwin; Linux/macOS nativos não foram executados.
- Testes unitários do wire format e remontagem.
- Executável real com três produtores UDP simulados e três leitores HTTP.
- Fragmentos reordenados, duplicados, perdidos, atrasados e malformados.
- Mudança de boot, rejeição de pacotes da sessão anterior, servidor errado e fonte desconhecida.
- Expiração sem depender da chegada do próximo frame.
- Cliente HTTP que não lê: não bloqueia consumidor saudável e é desconectado.
- Estados offline/reconexão e encerramento limpo.
- Reinício da ponte e nova ordem de registro preservam IDs/endpoints.
- Allowlist com vaga livre rejeita ID desconhecido; colisão de ID ativo em outro IP é rejeitada.
- Migração simulada entre endereços loopback após expiração do lease preserva o endpoint.

Os testes usam JPEGs estruturais sintéticos para verificar preservação de bytes, não a qualidade de uma imagem real. Nenhum dispositivo físico foi gravado durante a implementação. Resultados de compilação do firmware devem ser distinguidos de validação no sensor real.

Os dois testes CTest (`protocol` e `integration`) passaram. Os artefatos de firmware estão em `firmware/artifacts/`; o log do build local está em `build/firmware-build.log`. O wrapper de Windows usa um diretório temporário sem espaços. O script `firmware/pio_compat.py` adapta os arquivos de argumentos de compilação do ESP-IDF ao parser do PlatformIO/SCons, preservando opções como `-mlongcalls`, sem alterar os pacotes instalados.

## Roteiro no hardware

Atualização monocromática: firmware compilado com perfil OV2640 do OpenIris, JPEG 240×240 e envio UDP por dois segmentos em `sendmsg`. Ainda é necessário validar imagem, exposição, recepção dos datagramas e desempenho nas placas físicas. O teto de 90 FPS não é uma medição. Valores de tamanho do binário acima correspondem ao build inicial.

1. Compilar/gravar uma XIAO com o perfil OV2640; verificar identificação do sensor e PSRAM no serial.
2. Provisionar Wi-Fi, iniciar ponte e confirmar registro automático sem IP cadastrado.
3. Repetir nas outras duas placas; atribuir `left_eye`, `right_eye`, `face` por ID.
4. Abrir os URLs no consumidor HTTP MJPEG; confirmar orientação e resolução.
5. Desligar/religar uma câmera: as outras devem continuar; endpoint deve permanecer o mesmo.
6. Renovar DHCP e verificar retomada. Se o IP mudar durante lease ativo, aguardar até 6 s.
7. Encerrar/reiniciar a ponte preservando `pairings.txt`; as câmeras devem voltar automaticamente.
8. Ativar `auto_pair=false` e verificar que uma quarta origem desconhecida não é aceita.
9. Bloquear broadcast: deve aparecer diagnóstico serial; configurar IP manual e testar unicast.
10. Testar tracking no consumidor. Comparar com HTTP original sob condições equivalentes (1/2/3 câmeras).
11. Executar por horas, registrando CPU/RAM/FPS/perda/idade. Observar se há atraso crescente.
12. Repetir com PC em Ethernet, Wi-Fi em 20 MHz e sinal adequado. Ajustar qualidade/FPS/resolução só com medições.

O teto configurado é 90 FPS por câmera, não um resultado medido. O FPS efetivo depende do sensor, resolução, exposição e rede. Não há teste automatizado de interferência de rádio, DHCP real ou tracking.

## Decisões de implementação

- C++ no PC conforme pedido explícito do usuário, substituindo a sugestão de protótipo Python.
- Firmware independente em ESP-IDF, com captura `esp32-camera`, recriando o caminho mínimo de transporte. Não foi incorporado o firmware completo upstream.
- Perfil XIAO Sense/OV2640 informado pelo usuário, em vez de um ESP32S3 genérico.
- INI com validação estrita, sem dependência YAML.
- Descoberta UDP broadcast + unicast manual, alternativa prevista no documento. mDNS/DNS-SD não implementado.
- Pareamento local por MAC/ID do servidor e allowlist; sem criptografia.
- Controles USB para Wi-Fi/diagnóstico/limpeza de pareamento; sem dependência da API OpenIris no consumidor.
- Integração com Baballonia limitada à saída HTTP MJPEG genérica, conforme esclarecimento do usuário.

## Fontes consultadas

Consulta em 31/10/2026:

- [OpenIris-ESPIDF](https://github.com/EyeTrackVR/OpenIris-ESPIDF), commit `b64b0fa84ce0d1b073067f8758399d76033340ad`: README, componente de câmera e `boards/seed_studio/xiao_esp32s3`. Usado para conferir a arquitetura e o perfil; não é dependência de build.
- [Seeed: câmera XIAO ESP32S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/): pinagem do conector e configuração de captura/PSRAM.
- [Espressif esp32-camera](https://github.com/espressif/esp32-camera): driver de captura e API JPEG.
- [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html): estrutura de projeto e componentes gerenciados.
- [Ferramentas do ESP-IDF 5.5.5](https://github.com/espressif/esp-idf/blob/v5.5.5/tools/tools.json): confirma suporte ao compilador Xtensa `esp-14.2.0_20260121` existente neste computador. ESP-IDF 5.4.0 rejeitou essa versão na checagem de compatibilidade; a configuração foi alinhada ao SDK 5.5.5, sem desativar a checagem.
- [Baballonia](https://github.com/Project-Babble/Baballonia), commit `3f85119d786b57bd9f461177f71415c682aa9f4b`: `src/Baballonia.IPCameraCapture/IpCameraCaptureFactory.cs` aceita `http://`; `IPCameraCapture.cs` lê o stream e identifica SOI/EOI. Compatibilidade de entrada inspecionada no código, não teste executado de tracking.

As cópias de consulta em `.reference/` são ignoradas pelo Git e não fazem parte da aplicação distribuída. Licenças dos componentes externos continuam sendo as de seus respectivos projetos.
