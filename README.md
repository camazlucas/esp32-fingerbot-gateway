# Gateway Bluetooth ESP32 — Fingerbot para ligar o PC remotamente

Uma ESP32 conectada 24h no Wi-Fi de casa que funciona como ponte
Bluetooth → Wi-Fi: ela fica de olho num campo no Firebase e, quando ele
muda, aciona via Bluetooth Low Energy (BLE) um **Fingerbot** (robozinho que
aperta botões físicos) instalado no botão de power do PC. Objetivo: ligar o
PC de fora de casa, sem depender de o PC já estar ligado.

**Quer montar isso do zero? Veja o [guia passo a passo](GUIA-PASSO-A-PASSO.md).**

## Como funciona

```
[App MIT App Inventor]  --escreve trigger=true-->  [Firebase Realtime DB]
                                                            |
                                                   polling HTTP a cada 2s
                                                            |
                                                            v
                                                      [ESP32 (sempre ligada)]
                                                            |
                                                  Bluetooth Low Energy (BLE)
                                                            |
                                                            v
                                                      [Fingerbot no botão do PC]
```

1. Você aperta o botão no app (feito no MIT App Inventor) → ele grava
   `true` no Firebase Realtime Database.
2. A ESP32 faz polling HTTP nesse campo a cada 2 segundos.
3. Ao ver `true`, ela liga o Bluetooth, conecta no Fingerbot, faz o
   handshake do protocolo **Tuya BLE** (autenticação + criptografia AES) e
   manda o comando de "click".
4. Depois do click, zera o campo no Firebase de volta pra `false` e reinicia
   a si mesma (necessário: usar BLE nessa placa consome memória que o
   Wi-Fi/HTTPS não recupera sozinho até reiniciar).

## Estrutura das pastas

```
firmware/
  GatewayFingerbot/    <- O SCRIPT PRINCIPAL. É o único que roda na ESP32
                          no dia a dia (Wi-Fi + Firebase + Bluetooth juntos).
testes/
  TuyaFingerbotClick/  <- Sketch de teste, só do Bluetooth (sem Wi-Fi).
                          Usado pra debugar o Fingerbot isoladamente.
  test_wifi/           <- Sketch de teste, só do Wi-Fi + polling no
                          Firebase (primeira etapa do projeto).
```

Cada pasta acima é um **sketch do Arduino IDE** independente (abra a pasta
inteira, não só o `.ino`, pelo Arduino IDE). Só o `firmware/GatewayFingerbot`
precisa ficar rodando na ESP32; os dois de `testes/` existem só como
histórico e ferramenta de debug caso algo pare de funcionar.

## Hardware

- ESP32 **ESP-VROOM-32** (placa "DOIT ESP32 DEVKIT V1" / genérica "ESP32 Dev Module")
- Fingerbot Tuya BLE (testado com **CUBETOUCH II**)

## Configuração antes de compilar

Cada pasta de sketch tem um `secrets.h.example`. Copie para `secrets.h` (mesma
pasta) e preencha com seus dados reais — esse arquivo fica de fora do Git.
Detalhes de como conseguir cada dado estão no [guia passo a passo](GUIA-PASSO-A-PASSO.md).

## Configuração do Arduino IDE

- Board: **ESP32 Dev Module** (a entrada específica "DOIT ESP32 DEVKIT V1"
  não expõe o menu de partição necessário).
- Tools → Partition Scheme: **"No OTA (2MB APP/2MB SPIFFS)"** ou
  **"Minimal SPIFFS (1.9MB APP with OTA)"** — o firmware junta Wi-Fi+HTTPS e
  Bluetooth, que não cabem no esquema de partição padrão.

## ⚠️ Segurança pendente: regras do Firebase

Por padrão, um Firebase Realtime Database criado em "modo de teste" fica com
leitura/escrita **públicas** (qualquer pessoa com a URL consegue ler ou
escrever nele). Como esse projeto usa o Firebase só como um "interruptor"
remoto, o ideal é pelo menos restringir a escrita (ex.: regras que exigem
autenticação, ou trocar a URL do campo por algo não-adivinhável). Isso ainda
não foi configurado.

## Protocolo Tuya BLE (referências usadas)

- https://github.com/redphx/poc-tuya-ble-fingerbot (POC em Python, base da
  implementação em C++/Arduino deste repo)
- https://github.com/PlusPlus-ua/ha_tuya_ble (confirmação dos códigos do
  protocolo v3 e UUIDs GATT)
