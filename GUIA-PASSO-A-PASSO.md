# Guia passo a passo — montando o gateway do zero

Este guia assume que você já tem: a ESP32, o Fingerbot instalado
fisicamente no botão de power do PC, e o app Smart Life instalado no
celular.

## 1. Parear o Fingerbot no app Smart Life

Abra o app **Smart Life** (Tuya) e adicione o Fingerbot normalmente
(Bluetooth precisa estar ligado no celular). Confirme que consegue
acioná-lo pelo app antes de continuar.

## 2. Extrair as credenciais do Fingerbot

O Fingerbot fala um protocolo Bluetooth próprio da Tuya, criptografado. Pra
controlar ele com código próprio (fora do app), você precisa de 3 dados por
dispositivo: `device_id`, `local_key` e `uuid`.

1. Crie uma conta developer em [iot.tuya.com](https://iot.tuya.com).
2. Crie um **Cloud Project** (tipo "Smart Home").
3. Assine as APIs **IoT Core** e **Authorization** no projeto.
4. Vincule sua conta do app Smart Life ao projeto ("Link Tuya App
   Account", via QR code que aparece na plataforma).
5. Instale o [tinytuya](https://github.com/jasonacox/tinytuya)
   (`pip install tinytuya`) e rode:
   ```
   python -m tinytuya wizard
   ```
   Ele vai pedir a API Key/Secret do seu Cloud Project e vai gerar um
   `devices.json` com os dados de todos os seus dispositivos Tuya —
   inclusive o Fingerbot. Anote `id` (é o `device_id`), `key` (é o
   `local_key`), `uuid` e `mac`.

   ⚠️ Esse `devices.json` (e os outros arquivos que o tinytuya gera:
   `snapshot.json`, `tinytuya.json`, `tuya-raw.json`) contêm as chaves de
   **todos** os seus dispositivos Tuya, não só do Fingerbot. Eles já estão
   no `.gitignore` deste repositório — nunca suba eles pro GitHub.

## 3. Criar o Firebase Realtime Database

1. Crie um projeto em [console.firebase.google.com](https://console.firebase.google.com).
2. Ative o **Realtime Database** (modo de teste é suficiente pra começar —
   veja o aviso de segurança no README sobre deixar isso público).
3. Anote a URL do banco (algo como
   `https://SEU-PROJETO-default-rtdb.firebaseio.com`). O campo usado neste
   projeto é `fingerbot/trigger` (a URL completa fica
   `.../fingerbot/trigger.json`).

## 4. Montar o app no MIT App Inventor

1. Crie um projeto no [MIT App Inventor](https://appinventor.mit.edu).
2. Adicione o componente **Firebase DB** (na paleta "Experimental").
3. Configure a URL do Firebase no componente.
4. Adicione um botão que, ao ser clicado, chama
   `FirebaseDB.StoreValue("fingerbot/trigger", true)`.

   (O app não precisa se preocupar em voltar o valor pra `false` — quem
   faz isso é a própria ESP32, depois de acionar o Fingerbot com sucesso.)

## 5. Configurar o Arduino IDE

1. Instale o suporte a placas **ESP32** (Boards Manager, se ainda não tiver).
2. Em **Tools → Board**, selecione **"ESP32 Dev Module"** (não a entrada
   específica "DOIT ESP32 DEVKIT V1" — ela não expõe o menu de partição que
   o passo seguinte precisa).
3. Em **Tools → Partition Scheme**, escolha **"No OTA (2MB APP/2MB
   SPIFFS)"** ou **"Minimal SPIFFS (1.9MB APP with OTA)"**. O firmware
   combina Wi-Fi+HTTPS e Bluetooth, que juntos não cabem no esquema de
   partição padrão.
4. Selecione a porta COM correta em **Tools → Port**.

Nenhuma biblioteca extra precisa ser instalada — tudo usado
(`WiFi`, `HTTPClient`, `BLEDevice`, `MD5Builder`, `mbedtls`) já vem com o
core ESP32 do Arduino.

## 6. Preencher as credenciais (`secrets.h`)

Em `firmware/GatewayFingerbot/`, copie `secrets.h.example` para
`secrets.h` e preencha:

- `WIFI_SSID` / `WIFI_PASSWORD`: da sua rede Wi-Fi de casa.
- `FIREBASE_URL`: a URL completa do campo (passo 3).
- `FB_MAC`, `FB_UUID`, `FB_DEV_ID`, `FB_LOCAL_KEY`: do `devices.json`
  gerado no passo 2.

## 7. (Opcional) Testar as partes isoladamente

Se algo der errado no firmware completo, ajuda testar cada parte sozinha
primeiro — os dois sketches em `testes/` fazem isso:

- `testes/test_wifi/`: só conecta no Wi-Fi e fica lendo o campo do Firebase
  no Serial Monitor. Confirma que a rede e a URL do Firebase estão certas.
- `testes/TuyaFingerbotClick/`: só liga o Bluetooth e tenta acionar o
  Fingerbot direto, sem depender do Wi-Fi/Firebase. Confirma que as
  credenciais do Fingerbot estão certas.

Cada um também tem seu próprio `secrets.h.example` pra preencher.

## 8. Upload do firmware final

Abra a pasta `firmware/GatewayFingerbot` inteira no Arduino IDE, dê upload,
e acompanhe o Serial Monitor (115200 baud). Ele deve mostrar:

```
Conectando ao Wi-Fi....
Conectado!
Valor do trigger: false
```

repetindo a cada 2 segundos.

## 9. Teste end-to-end

Com a ESP32 rodando e perto o suficiente do Fingerbot (Bluetooth tem
alcance curto), aperte o botão no app. Em até 2 segundos a ESP32 deve:

1. Detectar `trigger: true`.
2. Conectar no Fingerbot via Bluetooth e mandar o clique.
3. Zerar o campo no Firebase.
4. Reiniciar sozinha (é esperado — ver README).

Se o Fingerbot mexer fisicamente, está tudo funcionando. Da próxima vez
não precisa estar perto do Arduino IDE nem do Serial Monitor — o gateway
roda sozinho, só precisa estar ligado na tomada e no Wi-Fi.
