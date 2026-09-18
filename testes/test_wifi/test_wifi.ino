#include <WiFi.h>
#include <HTTPClient.h>

// Credenciais em secrets.h, que NÃO entra no Git (veja .gitignore). Copie
// secrets.h.example pra secrets.h e preencha antes de compilar.
#include "secrets.h"

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado!");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(FIREBASE_URL);
    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      Serial.print("Valor do trigger: ");
      Serial.println(payload);
    } else {
      Serial.print("Erro na requisição: ");
      Serial.println(httpCode);
    }
    http.end();
  }

  delay(2000); // consulta a cada 2 segundos
}
