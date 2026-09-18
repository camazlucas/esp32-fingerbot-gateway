/*
  GatewayFingerbot
  -------------------
  Firmware completo: ESP32 conectada no Wi-Fi de casa, fazendo polling no
  Firebase Realtime Database. Quando o campo "fingerbot/trigger" vem `true`
  (o app escreve isso quando você aperta o botão), a ESP32 conecta via
  Bluetooth no Fingerbot, manda o comando de "click" (protocolo Tuya BLE) e
  depois zera o campo no Firebase de volta pra `false`.

  Junta duas partes já testadas separadamente:
    - test_wifi.ino: Wi-Fi + polling HTTP no Firebase.
    - TuyaFingerbotClick.ino: protocolo Tuya BLE (handshake + comando de click).
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <MD5Builder.h>
#include "mbedtls/aes.h"
#include <vector>
#include <map>

// Wi-Fi, Firebase e credenciais do Fingerbot ficam em secrets.h, que NÃO
// entra no Git (veja .gitignore). Copie secrets.h.example pra secrets.h e
// preencha com os seus dados antes de compilar.
#include "secrets.h"

// UUIDs das características GATT do Fingerbot (o serviço que as contém varia
// por firmware - por isso a busca abaixo varre todos os serviços do device)
static const char* CHARACTERISTIC_WRITE  = "00002b11-0000-1000-8000-00805f9b34fb";
static const char* CHARACTERISTIC_NOTIFY = "00002b10-0000-1000-8000-00805f9b34fb";

static const int GATT_MTU = 20; // tamanho máximo de cada pacote BLE que mandamos

// Códigos de mensagem do protocolo Tuya BLE
enum TuyaCode : uint16_t {
  CODE_DEVICE_INFO = 0x0000,
  CODE_PAIR        = 0x0001,
  CODE_DPS         = 0x0002,
};

// ---------------------------------------------------------------------------
// Estado da "conversa" com o Fingerbot
// ---------------------------------------------------------------------------
enum FlowState {
  ST_IDLE,
  ST_SENT_DEVICE_INFO,
  ST_GOT_DEVICE_INFO,
  ST_SENT_PAIR,
  ST_GOT_PAIR_ACK,
  ST_SENT_DPS,
  ST_DONE,
  ST_ERROR
};

volatile FlowState g_state = ST_IDLE;
uint32_t g_snAck = 0;               // contador de sequência das mensagens que enviamos
uint8_t  g_loginKey[6];             // primeiros 6 bytes do local_key
uint8_t  g_key4[16];                // chave usada só na 1ª mensagem (security_flag = 4)
uint8_t  g_key5[16];                // chave de sessão (security_flag = 5), calculada após receber o srand
uint32_t g_dpsSentAt = 0;

BLEClient* g_client = nullptr;
BLERemoteCharacteristic* g_writeChar = nullptr;
BLERemoteCharacteristic* g_notifyChar = nullptr;

// ---------------------------------------------------------------------------
// Buffer de remontagem dos pacotes recebidos (uma notificação BLE só carrega
// um pedaço; o Fingerbot manda vários pedaços que precisam ser "colados")
// ---------------------------------------------------------------------------
struct Receiver {
  std::vector<uint8_t> raw;
  uint32_t dataLength = 0;
  uint32_t currentLength = 0;
  int32_t lastIndex = -1;

  // devolve: 0 = pacote completo em 'raw', 1 = ainda faltam pedaços, <0 = erro
  int feed(const uint8_t* arr, size_t len) {
    size_t i = 0;
    uint32_t packetNumber = 0;
    while (i < 4 && i < len) {
      uint8_t b = arr[i];
      packetNumber |= (uint32_t)(b & 0x7F) << (i * 7);
      if (((b >> 7) & 1) == 0) break;
      i++;
    }
    size_t pos = i + 1;

    if (packetNumber == 0) {
      dataLength = 0;
      while (pos <= i + 4 && pos < len) {
        uint8_t b2 = arr[pos];
        dataLength |= (uint32_t)(b2 & 0x7F) << ((pos - 1 - i) * 7);
        if (((b2 >> 7) & 1) == 0) break;
        pos++;
      }
      currentLength = 0;
      lastIndex = -1;
      if (pos == i + 5 || len < pos + 2) return -1; // pacote malformado
      raw.clear();
      pos += 1; // pula byte de versão
      pos += 1;
    }

    if ((int32_t)packetNumber == 0 || (int32_t)packetNumber > lastIndex) {
      size_t dataLen = (pos < len) ? (len - pos) : 0;
      currentLength += dataLen;
      lastIndex = packetNumber;
      raw.insert(raw.end(), arr + pos, arr + pos + dataLen);
      if (currentLength < dataLength) return 1;
      return (currentLength == dataLength) ? 0 : -2;
    }
    return 1;
  }
};

Receiver g_receiver;

// ---------------------------------------------------------------------------
// Helpers de criptografia / checksum
// ---------------------------------------------------------------------------
void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
  MD5Builder md5b;
  md5b.begin();
  md5b.add((uint8_t*)data, len);
  md5b.calculate();
  md5b.getBytes(out);
}

// CRC16 padrão (Modbus/ARC), o mesmo usado pelo protocolo Tuya
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t idx = 0; idx < len; idx++) {
    crc ^= data[idx];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
      else crc >>= 1;
    }
  }
  return crc;
}

void aesCbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len, uint8_t* out) {
  uint8_t ivCopy[16]; memcpy(ivCopy, iv, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, ivCopy, in, out);
  mbedtls_aes_free(&ctx);
}

void aesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t len, uint8_t* out) {
  uint8_t ivCopy[16]; memcpy(ivCopy, iv, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, ivCopy, in, out);
  mbedtls_aes_free(&ctx);
}

void appendU32BE(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
  v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}
void appendU16BE(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

// ---------------------------------------------------------------------------
// Monta um pacote Tuya BLE completo (cabeçalho + payload + crc + criptografia)
// e manda pra característica de escrita, já fatiado em pedaços de 20 bytes.
// ---------------------------------------------------------------------------
void sendTuyaPacket(uint16_t code, uint8_t securityFlag, const uint8_t key[16],
                     const uint8_t* input, size_t inputLen) {
  g_snAck++;

  std::vector<uint8_t> raw;
  appendU32BE(raw, g_snAck);   // sn_ack: número da nossa mensagem
  appendU32BE(raw, 0);         // ack_sn: não estamos confirmando nada do dispositivo
  appendU16BE(raw, code);
  appendU16BE(raw, (uint16_t)inputLen);
  raw.insert(raw.end(), input, input + inputLen);
  uint16_t crc = crc16(raw.data(), raw.size());
  appendU16BE(raw, crc);

  while (raw.size() % 16 != 0) raw.push_back(0x00); // AES-CBC exige múltiplo de 16

  uint8_t iv[16];
  esp_fill_random(iv, 16); // vetor de inicialização aleatório, exigido pelo AES-CBC

  std::vector<uint8_t> encrypted(raw.size());
  aesCbcEncrypt(key, iv, raw.data(), raw.size(), encrypted.data());

  std::vector<uint8_t> packet;
  packet.push_back(securityFlag);
  packet.insert(packet.end(), iv, iv + 16);
  packet.insert(packet.end(), encrypted.begin(), encrypted.end());

  // Fatia em pedaços de até GATT_MTU bytes (limite de uma escrita BLE)
  size_t pos = 0, packetNumber = 0, total = packet.size();
  while (pos < total) {
    std::vector<uint8_t> chunk;
    chunk.push_back(packetNumber & 0xFF);
    size_t headerLen = 1;
    if (packetNumber == 0) {
      chunk.push_back(total & 0xFF);      // tamanho total do pacote (cabe em 1 byte, nossos pacotes são pequenos)
      chunk.push_back((2 << 4) & 0xFF);   // versão do protocolo (2) no nibble alto
      headerLen = 3;
    }
    size_t maxData = GATT_MTU - headerLen;
    size_t n = min(maxData, total - pos);
    chunk.insert(chunk.end(), packet.begin() + pos, packet.begin() + pos + n);

    g_writeChar->writeValue(chunk.data(), chunk.size(), false); // false = "write command", sem esperar resposta
    pos += n;
    packetNumber++;
    delay(20); // pequena folga entre pacotes BLE
  }
}

// ---------------------------------------------------------------------------
// Callback chamado sempre que o Fingerbot manda uma notificação BLE.
// Só decodifica/decripta aqui; o ENVIO da próxima mensagem é feito no loop()
// de fingerbotClick() (mandar dado de dentro do callback pode travar a pilha BLE).
// ---------------------------------------------------------------------------
void notifyCallback(BLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  int status = g_receiver.feed(data, length);
  if (status != 0) return; // ainda incompleto (1) ou erro (<0): ignora por enquanto

  uint8_t securityFlag = g_receiver.raw[0];
  const uint8_t* key = (securityFlag == 4) ? g_key4 : g_key5;

  size_t cipherLen = g_receiver.raw.size() - 17;
  std::vector<uint8_t> decrypted(cipherLen);
  aesCbcDecrypt(key, g_receiver.raw.data() + 1, g_receiver.raw.data() + 17, cipherLen, decrypted.data());

  uint16_t code = (decrypted[8] << 8) | decrypted[9];
  uint16_t payloadLen = (decrypted[10] << 8) | decrypted[11];
  const uint8_t* payload = decrypted.data() + 12;

  if (code == CODE_DEVICE_INFO && payloadLen >= 12) {
    // srand fica nos bytes 6..11 da resposta (ver DeviceInfoResp no protocolo)
    uint8_t srand[6];
    memcpy(srand, payload + 6, 6);

    uint8_t buf[12];
    memcpy(buf, g_loginKey, 6);
    memcpy(buf + 6, srand, 6);
    md5(buf, 12, g_key5); // chave de sessão: MD5(login_key + srand)

    Serial.println("[Tuya] device info recebido, srand obtido -> vou parear");
    g_state = ST_GOT_DEVICE_INFO;
  } else if (code == CODE_PAIR) {
    Serial.println("[Tuya] pareamento confirmado -> vou mandar o click");
    g_state = ST_GOT_PAIR_ACK;
  }
}

// ---------------------------------------------------------------------------
// Monta o payload do comando de click:
//   DP2 "mode"                = enum índice 0 ("click", único valor possível)
//   DP3 "click_sustain_time"  = inteiro (segundos que o dedo fica pressionado)
// ---------------------------------------------------------------------------
std::vector<uint8_t> buildClickPayload(uint8_t sustainSeconds) {
  std::vector<uint8_t> raw;
  raw.push_back(2); raw.push_back(4); raw.push_back(1); raw.push_back(0);      // DP2, tipo enum, tamanho 1, valor 0 = "click"
  raw.push_back(3); raw.push_back(2); raw.push_back(4);                        // DP3, tipo int, tamanho 4
  appendU32BE(raw, sustainSeconds);
  return raw;
}

// ---------------------------------------------------------------------------
// Conecta no Fingerbot via Bluetooth e manda o clique.
// Retorna true se o fluxo todo (handshake + pareamento + comando) deu certo.
// ---------------------------------------------------------------------------
bool fingerbotClick(uint8_t sustainSeconds = 2, uint32_t timeoutMs = 10000) {
  memcpy(g_loginKey, FB_LOCAL_KEY, 6);
  md5(g_loginKey, 6, g_key4);

  g_snAck = 0;
  g_state = ST_IDLE;
  g_receiver = Receiver();

  Serial.println("[Tuya] ligando Bluetooth e conectando no Fingerbot...");
  // Liga o Bluetooth só agora, na hora do clique. Ele fica desligado o
  // resto do tempo (ver fim da função) pra não brigar com o Wi-Fi por
  // memória/rádio - foi isso que deixava o HTTPS travado depois de um tempo.
  BLEDevice::init("");
  g_client = BLEDevice::createClient();
  if (!g_client->connect(BLEAddress(FB_MAC))) {
    Serial.println("[Tuya] falha ao conectar (dispositivo fora de alcance/desligado?)");
    BLEDevice::deinit(true);
    g_client = nullptr;
    return false;
  }

  // O UUID de serviço "oficial" no advertising nem sempre é onde as
  // características realmente vivem. Por isso procuramos em TODOS os
  // serviços do dispositivo até achar as duas que interessam.
  g_writeChar = nullptr;
  g_notifyChar = nullptr;
  std::map<std::string, BLERemoteService*>* services = g_client->getServices();
  for (auto& kv : *services) {
    BLERemoteService* svc = kv.second;
    if (!g_writeChar) g_writeChar = svc->getCharacteristic(BLEUUID(CHARACTERISTIC_WRITE));
    if (!g_notifyChar) g_notifyChar = svc->getCharacteristic(BLEUUID(CHARACTERISTIC_NOTIFY));
  }
  if (!g_writeChar || !g_notifyChar) {
    Serial.println("[Tuya] características BLE (2b10/2b11) não encontradas em nenhum serviço");
    g_client->disconnect();
    BLEDevice::deinit(true);
    g_client = nullptr;
    return false;
  }
  g_notifyChar->registerForNotify(notifyCallback);

  // Passo 1: pede as informações do dispositivo (abre o handshake)
  sendTuyaPacket(CODE_DEVICE_INFO, 4, g_key4, nullptr, 0);
  g_state = ST_SENT_DEVICE_INFO;

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (g_state == ST_GOT_DEVICE_INFO) {
      // Passo 2: manda pareamento (uuid + login_key + dev_id, com padding)
      std::vector<uint8_t> inp;
      inp.insert(inp.end(), FB_UUID, FB_UUID + strlen(FB_UUID));
      inp.insert(inp.end(), g_loginKey, g_loginKey + 6);
      inp.insert(inp.end(), FB_DEV_ID, FB_DEV_ID + strlen(FB_DEV_ID));
      while (inp.size() < (size_t)strlen(FB_UUID) + 6 + 22) inp.push_back(0x00);

      sendTuyaPacket(CODE_PAIR, 5, g_key5, inp.data(), inp.size());
      g_state = ST_SENT_PAIR;
    } else if (g_state == ST_GOT_PAIR_ACK) {
      // Passo 3: manda o comando de click
      std::vector<uint8_t> dps = buildClickPayload(sustainSeconds);
      sendTuyaPacket(CODE_DPS, 5, g_key5, dps.data(), dps.size());
      g_state = ST_SENT_DPS;
      g_dpsSentAt = millis();
    } else if (g_state == ST_SENT_DPS && millis() - g_dpsSentAt > 500) {
      g_state = ST_DONE;
      break;
    }
    delay(20);
  }

  bool ok = (g_state == ST_DONE);
  Serial.println(ok ? "[Tuya] click enviado com sucesso!" : "[Tuya] timeout - fluxo não terminou");

  g_client->disconnect();
  BLEDevice::deinit(true); // libera toda a memória do Bluetooth até o próximo clique
  g_client = nullptr;
  delay(300); // dá uma folga pro Wi-Fi "voltar" depois do uso do Bluetooth
  return ok;
}

// ---------------------------------------------------------------------------
// Firebase: lê o campo trigger, e depois de acionar o Fingerbot, zera ele.
// ---------------------------------------------------------------------------
bool readFirebaseTrigger() {
  HTTPClient http;
  http.begin(FIREBASE_URL);
  int httpCode = http.GET();

  bool triggered = false;
  if (httpCode == 200) {
    String payload = http.getString();
    payload.trim();
    Serial.print("Valor do trigger: ");
    Serial.println(payload);
    triggered = (payload.indexOf("true") >= 0); // aceita tanto `true` (booleano) quanto `"true"` (texto)
  } else {
    Serial.print("Erro na requisição: ");
    Serial.println(httpCode);
  }
  http.end();
  return triggered;
}

void resetFirebaseTrigger() {
  // Tenta algumas vezes: logo depois de usar o Bluetooth o Wi-Fi pode
  // falhar numa primeira tentativa (as duas rádios dividem o mesmo hardware).
  for (int tentativa = 1; tentativa <= 3; tentativa++) {
    HTTPClient http;
    http.begin(FIREBASE_URL);
    int httpCode = http.PUT("false"); // grava "false" de volta no campo
    http.end();
    if (httpCode == 200) return;

    Serial.print("Erro ao resetar trigger (tentativa ");
    Serial.print(tentativa);
    Serial.print("): ");
    Serial.println(httpCode);
    delay(500);
  }
  Serial.println("Não consegui resetar o trigger - vai tentar de novo no próximo ciclo");
}

// ---------------------------------------------------------------------------
// setup() / loop()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado!");
  // O Bluetooth só é ligado na hora do clique (dentro de fingerbotClick) e
  // desligado logo depois - deixá-lo sempre ligado junto com o Wi-Fi consome
  // memória que o HTTPS precisa, e o Wi-Fi trava depois de um tempo.
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (readFirebaseTrigger()) {
      bool ok = fingerbotClick(2);
      if (ok) resetFirebaseTrigger();
      else Serial.println("Click falhou, vou tentar de novo no próximo polling (sem zerar o trigger)");

      // O Bluetooth dessa placa "rouba" memória do Wi-Fi e não devolve
      // (limitação conhecida da lib), então reiniciamos a ESP32 depois de
      // cada uso do Bluetooth pra garantir que o Wi-Fi volte limpo. Como o
      // trigger já foi zerado (se deu certo), não reaciona nada ao religar.
      Serial.println("Reiniciando a ESP32 pra manter o Wi-Fi saudável...");
      delay(500);
      ESP.restart();
    }
  } else {
    Serial.println("Wi-Fi caiu, tentando reconectar...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  delay(2000); // consulta a cada 2 segundos
}
