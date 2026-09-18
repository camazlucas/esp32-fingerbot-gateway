# Gateway Bluetooth ESP32 — Fingerbot para ligar o PC remotamente

## Objetivo
Usar uma ESP32 (modelo **ESP-VROOM-32**) como gateway Bluetooth→Wi-Fi pra acionar um **Fingerbot** (robô que aperta botão físico) já instalado no botão de power do PC. Meta final: ligar o PC de fora de casa, sem depender do próprio PC estar ligado (então nada de Home Assistant rodando nele).

## Contexto do Fingerbot
- Já pareado no app **Smart Life** (Tuya).
- Fingerbots geralmente são dispositivos **Tuya BLE** — comunicação criptografada, não é um GATT simples de "escreve 1 byte e ativa".
- Pra controlar via código próprio (fora do app), é preciso extrair 3 credenciais por dispositivo: `device_id`, `local_key`, `uuid`.
- Extração: criar conta developer na **Tuya IoT Platform** (iot.tuya.com) → criar Cloud Project (tipo Smart Home) → assinar APIs IoT Core/Authorization → linkar a conta do app Smart Life ao projeto (Link Tuya App Account, via QR code) → rodar `python -m tinytuya wizard` (ou tuya-local-key-extractor) pra puxar os valores.
- Referências de protocolo Tuya BLE (v3 mapeado, v4 incerto):
  - https://github.com/redphx/poc-tuya-ble-fingerbot (POC em Python)
  - https://community.home-assistant.io/t/tuya-ble-integration-includes-fingerbot/562888 (integração HA)

## Arquitetura decidida
- **ESP32 sempre ligada** na tomada, conectada no Wi-Fi de casa.
- ESP32 atua como **BLE Central**, conecta no Fingerbot e implementa o handshake/protocolo Tuya BLE pra mandar o comando de "clique".
- **Ponte remota escolhida: Firebase Realtime Database + app feito no MIT App Inventor** (em vez de MQTT, ntfy.sh, port-forward/DDNS ou VPN/Tailscale — essas opções foram avaliadas e descartadas por ora por complexidade).
  - Fluxo: app (App Inventor) escreve um valor no Firebase → ESP32 faz polling via HTTP no Firebase → ao detectar mudança, aciona o Fingerbot via BLE.
  - Motivo da escolha: App Inventor tem componente nativo de Firebase, é mais visual/simples pro usuário configurar do que MQTT, e ele já pretende montar o app nele.
- Alternativas descartadas/adiadas:
  - MQTT com broker na nuvem — mais robusto/padrão, mas usuário não conhecia o conceito; pode ser revisitado depois se Firebase não performar bem.
  - Tailscale na ESP32 — existe suporte experimental recente (projeto MicroLink, github.com/CamM2325/microlink), mas é complexo demais pra começar.
  - Port forward + DDNS / VPN WireGuard — mais trabalho de configurar rede/roteador.

## Restrições do usuário (perfil)
- Lucas não tem conhecimento profundo em ESP32 — explicar termos técnicos ao introduzi-los.
- Preferência por respostas curtas.

## Próximos passos
1. Extrair `device_id`, `local_key`, `uuid` do Fingerbot via Tuya IoT Platform.
2. Definir/implementar o handshake do protocolo Tuya BLE no firmware da ESP32 (Arduino ou ESP-IDF).
3. Configurar projeto Firebase Realtime Database.
4. Montar app simples no MIT App Inventor (botão → escreve no Firebase).
5. Implementar no firmware da ESP32: Wi-Fi + polling HTTP no Firebase + trigger BLE no Fingerbot.
6. Testar end-to-end (app fora de casa → Firebase → ESP32 → Fingerbot → PC liga).