#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ------------------- CONFIGURACIÓN -------------------
#define BUZZER_PIN 26
#define BTN_SILENCIAR 33
#define MAX_ALARMAS 5

LiquidCrystal_I2C lcd(0x27, 20, 4); 
RTC_DS3231 rtc;     

// MAC del maestro
uint8_t masterAddress[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

// ------------------- ESTRUCTURAS -------------------
typedef struct {
  char nombre[20];
  char camilla[5];
  char medicamento[20];
  char dosis[5];
  char hora[6];
} AlarmaPaciente;

typedef struct {
  int year, month, day, hour, minute, second;
} HoraSync;

// ------------------- VARIABLES GLOBALES -------------------
AlarmaPaciente colaAlarmas[MAX_ALARMAS];
int tope = -1;
bool silenciada = false;
bool necesitaActualizarDisplay = true; // ✅ NUEVA BANDERA PARA CONTROLAR ACTUALIZACIONES

struct ControlAlarma {
  unsigned long inicioAlarma = 0;
  unsigned long ultimoRecordatorio = 0;
  bool buzzerActivo = false;
  static const unsigned long TIMEOUT_BUZZER = 60000; // 1 minuto
  static const unsigned long INTERVALO_RECORDATORIO = 30000; // 30 segundos
};
ControlAlarma controlAlarma;

// ------------------- FUNCIONES DE COLA -------------------
bool colaVacia() { return tope == -1; }
bool colaLlena() { return tope == MAX_ALARMAS - 1; }

void encolarAlarma(const AlarmaPaciente& alarma) {
  if (!colaLlena()) {
    tope++;
    memcpy(&colaAlarmas[tope], &alarma, sizeof(AlarmaPaciente));
    Serial.println("📥 Alarma encolada. Posición: " + String(tope));
  }
}

void desencolarAlarma() {
  if (!colaVacia()) {
    tope--;
    Serial.println("📤 Alarma desencolada. Nueva posición: " + String(tope));
  }
}

AlarmaPaciente* obtenerAlarmaActual() {
  return colaVacia() ? NULL : &colaAlarmas[tope];
}

int colaLongitud() { return tope + 1; }

// ------------------- MANEJO PERFECTO DEL DISPLAY -------------------
void mostrarSistemaNormal() {
  lcd.clear(); // ✅ LIMPIAR ANTES DE MOSTRAR
  
  DateTime now = rtc.now();
  
  // Línea 1: Fecha y Hora
  char fecha[11], hora[6];
  sprintf(fecha, "%02d/%02d/%04d", now.day(), now.month(), now.year());
  sprintf(hora, "%02d:%02d", now.hour(), now.minute());
  
  lcd.setCursor(0, 0); lcd.print(fecha);
  lcd.setCursor(14, 0); lcd.print(hora);
  
  // Líneas 2-4: Mensaje de sistema normal
  lcd.setCursor(0, 1); lcd.print("                    ");
  lcd.setCursor(0, 2); lcd.print("   SISTEMA NORMAL   ");
  lcd.setCursor(0, 3); lcd.print("                    ");
  
  necesitaActualizarDisplay = false; // ✅ SE ACTUALIZÓ CORRECTAMENTE
  Serial.println("📟 Modo normal activado");
}

void mostrarAlarmaActual() {
  lcd.clear(); // ✅ LIMPIAR ANTES DE MOSTRAR
  
  if (colaVacia()) {
    necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN INMEDIATA
    return;
  }
  
  AlarmaPaciente* actual = obtenerAlarmaActual();
  
  // Línea 1: ALERTA PACIENTE!(+X)
  lcd.setCursor(0, 0); 
  lcd.print("ALERTA PACIENTE");
  if (colaLongitud() > 1) { 
    lcd.print("(+");
    lcd.print(colaLongitud() - 1);
    lcd.print(")");
  }

  // Añadir estado abreviado (S o R)
  // Calcular posición final en la línea 20 (índice 19)
  lcd.setCursor(19, 0);  
  if (silenciada) {
    // Si está silenciada, mostrar S o R dependiendo del buzzer
    if (controlAlarma.buzzerActivo == false) {
      lcd.print("S"); // Silenciada manualmente
    } else {
      lcd.print("R"); // Recordatorio
    }
  } else {
    lcd.print(" "); // En blanco si está sonando
  }

    // Línea 2: Nombre del paciente
  lcd.setCursor(0,1); lcd.print(""); lcd.print(actual->nombre);
  lcd.setCursor(0,2); lcd.print("Camilla: "); lcd.print(actual->camilla);
  lcd.setCursor(0,3); lcd.print("Med: "); lcd.print(actual->medicamento);
  
  necesitaActualizarDisplay = false; // ✅ SE ACTUALIZÓ CORRECTAMENTE
  Serial.println("🚨 Mostrando alarma: " + String(actual->nombre));
}

// ------------------- CONTROL DE BUZZER CORREGIDO -------------------
void controlarBuzzer() {
  if (colaVacia()) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  unsigned long tiempoActual = millis();
  
  if (silenciada) {
    // MODO RECORDATORIO: Suena cada 30 segundos
    if (tiempoActual - controlAlarma.ultimoRecordatorio > ControlAlarma::INTERVALO_RECORDATORIO) {
      // Hacer 2 bips rápidos
      for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); 
        delay(100);
        digitalWrite(BUZZER_PIN, LOW); 
        delay(100);
      }
      controlAlarma.ultimoRecordatorio = tiempoActual;
      Serial.println("🔔 Recordatorio sonando");
    }
  } 
  else if (controlAlarma.buzzerActivo) {
    if (tiempoActual - controlAlarma.inicioAlarma <= ControlAlarma::TIMEOUT_BUZZER) {
      // Primer minuto: bip-bip cada 3 segundos
      unsigned long ciclo = (tiempoActual - controlAlarma.inicioAlarma) % 3000;
      
      if (ciclo < 100) digitalWrite(BUZZER_PIN, HIGH);
      else if (ciclo < 200) digitalWrite(BUZZER_PIN, LOW);
      else if (ciclo < 300) digitalWrite(BUZZER_PIN, HIGH);
      else digitalWrite(BUZZER_PIN, LOW);
    } else {
      // Timeout alcanzado - activar modo recordatorio
      controlAlarma.buzzerActivo = false;
      silenciada = true;
      controlAlarma.ultimoRecordatorio = tiempoActual; // Iniciar contador recordatorio
      necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN
      Serial.println("⏰ Timeout - Cambiando a modo recordatorio");
    }
  }
}

// ------------------- MANEJO DE ALARMAS -------------------
void activarNuevaAlarma(const AlarmaPaciente& nuevaAlarma) {
  encolarAlarma(nuevaAlarma);
  
  silenciada = false;
  controlAlarma.inicioAlarma = millis();
  controlAlarma.buzzerActivo = true;
  controlAlarma.ultimoRecordatorio = millis();
  
  necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN
  Serial.println("🚨 Nueva alarma: " + String(nuevaAlarma.nombre));
}

void confirmarAlarmaActual() {
  if (colaVacia()) return;
  
  desencolarAlarma();
  digitalWrite(BUZZER_PIN, LOW);
  silenciada = false;
  controlAlarma.buzzerActivo = false;
  
  Serial.println("✅ Alarma confirmada");
  
  necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN INMEDIATA
  
  if (!colaVacia()) {
    // Mostrar siguiente alarma
    controlAlarma.inicioAlarma = millis();
    controlAlarma.buzzerActivo = true;
    controlAlarma.ultimoRecordatorio = millis();
  } else {
    // Volver a sistema normal - LIMPIAR COMPLETAMENTE
    controlAlarma.buzzerActivo = false;
    silenciada = false;
    Serial.println("📟 Volviendo a sistema normal");
  }
}

void silenciarAlarmaActual() {
  if (!colaVacia() && !silenciada) {
    silenciada = true;
    controlAlarma.buzzerActivo = false;
    controlAlarma.ultimoRecordatorio = millis(); // Iniciar contador para recordatorios
    digitalWrite(BUZZER_PIN, LOW);
    necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN
    Serial.println("🔇 Alarma silenciada - Modo recordatorio activado");
  }
}

// ------------------- ESP-NOW -------------------
void enviarMensaje(const char* msg) {
  esp_now_send(masterAddress, (uint8_t*)msg, strlen(msg)+1);
}

void recibirDatos(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len < 20) {
    String msg = String((char*)incomingData);
    if (msg == "SILENCIAR") {
      silenciarAlarmaActual();
      Serial.println("🔇 Silenciar desde maestro");
      return;
    } else if (msg == "CONFIRMADA") {
      confirmarAlarmaActual();
      Serial.println("✅ Confirmada desde maestro");
      return;
    }
  }

  if (len == sizeof(AlarmaPaciente)) {
    AlarmaPaciente nuevaAlarma;
    memcpy(&nuevaAlarma, incomingData, sizeof(nuevaAlarma));
    activarNuevaAlarma(nuevaAlarma);
  } else if (len == sizeof(HoraSync)) {
    HoraSync sync;
    memcpy(&sync, incomingData, sizeof(sync));
    if (rtc.begin()) {
      rtc.adjust(DateTime(sync.year, sync.month, sync.day, sync.hour, sync.minute, sync.second));
      Serial.println("⏰ RTC sincronizado");
      if (colaVacia()) {
        necesitaActualizarDisplay = true; // ✅ FORZAR ACTUALIZACIÓN
      }
    }
  }
}

// ------------------- SETUP -------------------
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_SILENCIAR, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // Inicializar RTC
  if (!rtc.begin()) {
    Serial.println("❌ RTC no detectado");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ERROR RTC");
  } else {
    necesitaActualizarDisplay = true; // ✅ FORZAR PRIMERA ACTUALIZACIÓN
  }

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error ESP-NOW");
    while(true);
  }

  esp_now_register_recv_cb(recibirDatos);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ No se pudo agregar maestro");
  }

  Serial.println("✅ Esclavo listo");
}

// ------------------- LOOP CORREGIDO -------------------
void loop() {
  static unsigned long lastBtnCheck = 0;
  static unsigned long lastRTCUpdate = 0;

  // ✅ ACTUALIZACIÓN INMEDIATA DEL DISPLAY CUANDO SEA NECESARIO
  if (necesitaActualizarDisplay) {
    if (colaVacia()) {
      mostrarSistemaNormal();
    } else {
      mostrarAlarmaActual();
    }
  }

  controlarBuzzer(); // ✅ AHORA FUNCIONA EL MODO RECORDATORIO

  // Botón silenciar
  if (millis() - lastBtnCheck > 200) {
    if (!colaVacia() && digitalRead(BTN_SILENCIAR) == LOW) {
      silenciarAlarmaActual();
      enviarMensaje("SILENCIAR");
      delay(300); // Debounce
      lastBtnCheck = millis();
    }
    lastBtnCheck = millis();
  }

  // Actualizar hora solo en modo normal
  if (colaVacia() && rtc.begin()) {
    if (millis() - lastRTCUpdate > 1000) {
      // Solo actualizar la hora, no redibujar toda la pantalla
      DateTime now = rtc.now();
      char hora[6];
      sprintf(hora, "%02d:%02d", now.hour(), now.minute());
      lcd.setCursor(14, 0);
      lcd.print(hora);
      lastRTCUpdate = millis();
    }
  }

  delay(10);
}
