# Firmware ESP-IDF — XIAO ESP32S3 Sense / OV2640

O perfil assume a expansão de câmera **Sense** com OV2640 usando o conector e a pinagem padrão da Seeed. Uma XIAO sem essa expansão ou com câmera conectada por fios precisa de um perfil próprio; não grave assumindo outra pinagem.

## Hardware e parâmetros

| Item | Configuração |
|---|---|
| Placa PlatformIO | `seeed_xiao_esp32s3` |
| Flash | 8 MB, DIO |
| PSRAM | 8 MB, octal, 80 MHz |
| Sensor | OV2640, JPEG nativo com efeito monocromático fixo |
| Resolução | 240 × 240, conforme perfil OpenIris |
| XCLK | 20 MHz |
| Qualidade JPEG | 10 (escala do driver 0–63, menor = maior qualidade) |
| FPS máximo | 90, limitado também pela autorização do PC |
| Buffers | 2 na PSRAM, `CAMERA_GRAB_LATEST` |
| Wi-Fi | Station, power-save desativado |
| USB | Console USB Serial/JTAG |

Pinagem em `include/xiao_sense.hpp`: D0..D7 = 15,17,18,16,14,12,11,48; XCLK=10; PCLK=13; VSYNC=38; HREF=47; SDA=40; SCL=39; RESET/PWDN=-1. Configuração de imagem, espelhamento e destino manual em `include/settings.hpp`.

O perfil fixo em `include/ov2640_profile.hpp` segue os valores de sensor do [OpenIris-ESPIDF CameraManager](https://github.com/EyeTrackVR/OpenIris-ESPIDF/blob/b64b0fa84ce0d1b073067f8758399d76033340ad/components/CameraManager/CameraManager/CameraManager.cpp): efeito grayscale 2, saturação -2, brilho/contraste 2, exposição manual 300 e ganho manual 2, além dos ajustes de correção e clock JPEG. A exposição fixa pressupõe iluminação adequada ao uso; não foi calibrada nas suas câmeras. Não há opção de vídeo colorido.

Para este transporte, mantemos qualidade JPEG 10 (o upstream usa 8), dois buffers na PSRAM e `CAMERA_GRAB_LATEST`, priorizando banda e frames recentes. O sensor continua gerando JPEG; não há conversão por software na ESP32 ou no PC. O envio usa `sendmsg` com cabeçalho e fragmento separados, removendo uma cópia intermediária na aplicação; a pilha de rede ainda pode copiar dados. O protocolo e o payload de 1.200 bytes permanecem iguais. A redução de tráfego e o FPS real precisam ser medidos no hardware.

## Compilar

Abra esta pasta no PlatformIO ou execute a partir da raiz do repositório:

```powershell
pio run -d firmware -e xiao_esp32s3_ov2640
pio run -d firmware -e xiao_esp32s3_ov2640 -t upload --upload-port COM7
pio device monitor -p COM7 -b 115200
```

No Windows, para o caminho atual que contém espaços, use o wrapper que prepara uma cópia de build em uma pasta temporária sem espaços:

```powershell
.\tools\build-firmware.ps1
.\tools\build-firmware.ps1 -Action upload -Port COM7
.\tools\build-firmware.ps1 -Action monitor -Port COM7
```

Substitua COM7 pela porta real (`pio device list`). A gravação não foi executada automaticamente. Se necessário, segure BOOT ao conectar USB e solte para entrar no bootloader.

O projeto carrega automaticamente `platformio.local.ini` quando esse arquivo existe, inclusive pelo PlatformIO do VS Code. O wrapper também o reconhece. Nesta máquina, ele aponta para o compilador Xtensa 14.2.0+20260121 já instalado. Esse arquivo é ignorado pelo Git por conter um caminho específico do computador. Sem ele, usa-se a configuração padrão fixada em `platformio.ini`.

Plataforma fixada em `espressif32@6.13.0`, com overrides explícitos para **ESP-IDF 5.5.5** (`framework-espidf@4.50505.0`) e **Xtensa GCC 14.2.0+20260121**. Essa combinação usa o compilador já existente nesta máquina e aceito pelo SDK. O compilador RISC-V está fixado em `14.2.0+20241119`, mas não participa do firmware: o coprocessador ULP está desativado. Não habilite ULP sem alinhar também esse toolchain com o SDK.

Driver gerenciado `espressif/esp32-camera@2.0.16`; o componente é baixado pelo Component Manager do ESP-IDF, não pelo framework Arduino. Preserve `dependencies.lock` gerado para reproduzir a resolução das dependências.

O script de projeto `pio_compat.py` adapta os arquivos de argumentos de compilação do SDK ao PlatformIO/SCons: corrige as aspas e expande os argumentos antes de classificá-los. Isso preserva `-mlongcalls` e as demais opções necessárias, inclusive no bootloader. Não altera a instalação do SDK ou do PlatformIO.

ESP-IDF/PlatformIO não trata corretamente alguns argumentos com espaços no diretório de build; caminhos 8.3 também são expandidos pelo CMake. O wrapper copia os arquivos necessários para `%TEMP%\iris-udp-<identificador>`, preservando os fontes originais, e mantém ali o cache de compilação. Após sucesso, copia `firmware.bin`, `firmware.elf`, `bootloader.bin` e `partitions.bin` para `firmware/artifacts/`, além de atualizar `dependencies.lock`. Se usar `secrets.h`, ele também estará na cópia temporária. Outra opção é manter o projeto completo em um caminho sem espaços, como `C:\src\LocalImageProcessor`, preservando `common` ao lado de `firmware`.

## Wi-Fi antes de compilar

Edite `firmware/wifi.ini` (ou copie `wifi.example.ini` para esse nome):

```ini
[wifi]
ssid = MinhaRede
password = MinhaSenha
```

Use uma rede de 2,4 GHz e valores sem aspas. Caracteres como `%`, `#`, `;`, aspas e barras dentro do valor são preservados; espaços nas extremidades são removidos pelo leitor INI. Compile/grave normalmente pelo wrapper. O build gera um header privado, sem colocar a senha nos argumentos do compilador. O INI e o header gerado são ignorados pelo Git, mas as credenciais ficam no binário e na cópia temporária de build.

Cada compilação gera um identificador novo. No primeiro boot desse firmware, o SSID e a senha do INI substituem os valores antigos da NVS. Depois disso, alterações por USB persistem nos reinícios normais. Compilar e gravar novamente reaplica o INI, mesmo que ele não tenha sido editado. Regravar exatamente o mesmo binário já aplicado não reaplica os valores. SSID vazio desativa essa substituição e mantém a configuração por USB (ou o antigo `secrets.h`, se existir).

## Trocar o Wi-Fi por USB, persistido em NVS

O firmware sem credenciais inicia a console e aguarda configuração. Envie uma linha JSON terminada por Enter:

```json
{"command":"wifi","ssid":"MinhaRede","password":"MinhaSenha"}
```

O firmware grava em NVS e reinicia. As credenciais não são impressas pelo firmware. Use um terminal serial com envio de linha e eco local desativado se não quiser exibir a senha na tela. O monitor e a ferramenta de upload não podem ocupar a mesma porta simultaneamente.

Outros comandos:

```json
{"command":"status"}
{"command":"scan"}
{"command":"unpair"}
```

`status` informa ID, boot, Wi-Fi e heap livre. `unpair` apaga somente o ID do servidor e reinicia; mantém o Wi-Fi. Se `preferred_server` estiver fixado no código, continuará selecionando esse servidor.

Alternativa para primeiro boot: copie `include/secrets.example.h` para `include/secrets.h` e preencha SSID/senha. Esse arquivo é ignorado pelo Git. Credenciais já gravadas em NVS prevalecem sobre os valores iniciais. Não publique binários que contenham credenciais. Não é necessário colocar senhas no `platformio.ini`.

`status` também mostra o SSID configurado, sem a senha. `scan` interrompe temporariamente a conexão e lista até 20 redes detectadas, com canal, RSSI e modo de autenticação numérico do ESP-IDF. Ao terminar, retoma as tentativas de conexão. Execute depois de configurar Wi-Fi; o comando não altera as credenciais.

## Operação

- ID estável derivado dos seis bytes do MAC Wi-Fi. ID de boot aleatório a cada reinício.
- Descoberta/heartbeat UDP a cada 1 s, separados da tarefa de captura/envio.
- Primeiro ACK válido faz pareamento persistente. Para impedir pareamento com outro PC durante a configuração inicial, use `preferred_server`.
- Vídeo só é enviado enquanto houver ACK recebido nos últimos 4 s.
- Socket de vídeo não bloqueante; falha em qualquer fragmento abandona o restante do frame.
- Reconexão automática do Wi-Fi; descoberta continua após obter novo IP.
- Console mantém configuração e diagnóstico disponíveis, inclusive antes de configurar Wi-Fi.
- Sem stream HTTP na ESP32. Toda saída HTTP MJPEG fica no PC.

A detecção do sensor é feita pelo driver e o PID aparece no log. Falha de inicialização de câmera é reportada e tenta novamente em 5 s. Confirme PSRAM, alimentação, sensor e cabo flat se houver falha. Não foram implementados UVC, OTA, mDNS ou comandos REST do OpenIris original.
