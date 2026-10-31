# OIRB v1 — formato binário

Todos os inteiros são unsigned, em **network byte order / big endian**. Não se transmite uma struct C++ crua. Encoder/decoder único: `common/protocol.hpp`.

| Offset | Bytes | Campo |
|---:|---:|---|
| 0 | 4 | ASCII `OIRB` |
| 4 | 1 | Versão = 1 |
| 5 | 1 | Tipo: HELLO=1, ACCEPT=2, JPEG=3 |
| 6 | 2 | Payload length |
| 8 | 8 | Camera ID: MAC de 48 bits, dois bytes superiores zero |
| 16 | 4 | Boot ID, aleatório e não zero |
| 20 | 4 | Frame ID |
| 24 | 8 | Timestamp da captura em microssegundos no relógio do dispositivo |
| 32 | 2 | Índice do fragmento, base zero |
| 34 | 2 | Quantidade de fragmentos |
| 36 | 4 | Tamanho total do JPEG |
| 40 | 8 | ID persistente do servidor |
| 48 | variável | Payload |

Comprimento UDP exato = 48 + payload length. Até 1200 bytes de payload. IPv4 + UDP + OIRB + payload = 1276 bytes no maior pacote, abaixo do MTU usual de 1500. Redes com MTU inferior ainda precisam ser avaliadas.

## Controle, porta 5001

**HELLO:** câmera envia de socket efêmero para broadcast `255.255.255.255:5001` ou IP manual. Camera/boot são obrigatórios. Server ID=0 permite primeiro pareamento; depois contém o ID persistido. Payload opcional de até 128 bytes, informativo, por exemplo `xiao-esp32s3-sense;OV2640;iris-udp/1.0;JPEG;240x240;monochrome`. Frame/index/count/total devem ser zero na origem; o receptor não usa esses campos de controle.

**ACCEPT:** resposta unicast à origem do HELLO. Repete camera/boot, informa servidor não zero. Payload de 4 bytes: porta UDP de vídeo uint16 + FPS máximo uint16 (1..90). O firmware usa o menor limite entre seu máximo local e o oferecido. Não há retransmissão explícita; HELLO se repete a cada segundo e renova a presença. O PC mantém o registro por ID, não por IP.

Um registro mantém autorização por 6 s no PC. Uma câmera para de transmitir após 4 s sem ACCEPT. Troca de boot limpa a remontagem e o frame publicado anterior. Um IP diferente para um ID ativo é rejeitado até expirar a presença anterior. Esse mecanismo detecta colisões simples, mas não prova identidade nem impede spoofing.

## Vídeo, porta 5000

Cada fragmento traz camera/boot/server correspondentes ao registro. `total` entre 4 e 262144; `count=ceil(total/1200)`; `index<count`. Todos os fragmentos, exceto o último, contêm 1200 bytes. O último contém o restante exato. Tamanho, quantidade e timestamp permanecem iguais em todos os fragmentos do frame.

IDs de frame usam ordenação modular de 32 bits. A diferença deve ser menor que 2³¹ para ser considerada mais nova. Um boot novo permite reiniciar IDs. Um frame mais novo abandona o anterior mesmo incompleto. Timeout de remontagem é contado a partir do primeiro fragmento observado e não é prorrogado por duplicatas. Um frame expirado não pode ser ressuscitado por fragmentos atrasados.

JPEG só é publicado quando todos os fragmentos únicos chegam e os marcadores inicial `FFD8` e final `FFD9` existem. A validação não garante decodificação de todo o JPEG. Não há CRC adicional, FEC ou retransmissão nesta versão. O timestamp é transportado, mas não é interpretado como latência absoluta.

## HTTP local

`GET /camera/N.mjpeg`, N=1..3 pareado. Resposta `200`, `Content-Type: multipart/x-mixed-replace; boundary=frame`, cache desativado, conexão fechada ao encerrar. Cada parte:

```text
--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <bytes>\r\n
\r\n
<JPEG>\r\n
```

Não há Content-Length global nem codificação HTTP chunked. Não é necessário um delimitador final em uma conexão interrompida. Cliente lento tem deadline de envio de 1 s por parte; não há fila de frames por cliente.
