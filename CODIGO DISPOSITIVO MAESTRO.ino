// ===================== SISTEMA INTEGRADO: MENU + VER PACIENTES + NUEVO REGISTRO + CONFIG =====================
// HW: ESP32 + TFT ILI9488 (TFT_eSPI) + RTC DS3231 (SDA=25, SCL=14) + Buzzer pin 26 + Botón confirmación pin 33

#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <RTClib.h>
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;

#include <XPT2046_Touchscreen.h>
// Pines para pantalla táctil
#define TOUCH_CS 21     
#define TIRQ_PIN -1  // si no usas interrupción

XPT2046_Touchscreen ts(TOUCH_CS, TIRQ_PIN);

// MAC del esclavo remoto (anunciador)
uint8_t slaveAddress[] = {0xC8, 0xC9, 0xA3, 0xC7, 0x98, 0x90}; 

#define X_MIN 570
#define X_MAX 3784
#define Y_MIN 467
#define Y_MAX 3575

TFT_eSPI tft = TFT_eSPI();
RTC_DS3231 rtc;

// ---------------------------- Pines ----------------------------
#define BUZZER_PIN       26
#define BTN_CONFIRMAR    32         // Botón físico para silenciar la alarma (INPUT_PULLUP)

const String BT_CONFIRM_CMD = "CONFIRMAR";
TS_Point touch;

// ------------------------- Pacientes ---------------------------
struct Paciente {
  String nombre;
  String camilla;
  String medicamento;
  String dosis;
  int    frecuenciaHoras;         // cada cuántas horas
  String horaInicio;              // próxima alarma HH:MM
  DateTime primeraDosis;
  DateTime ultimaToma;            // última toma registrada
  DateTime proxima;   
  DateTime fechaInicioTratamiento;              
  int indiceHistorial = 0;      
  bool   alarmaActiva = false;    // si está sonando/parpadeando
  bool silenciada = false; // <-- añadir aquí
  unsigned long ultimoBip = 0;    // cadencia de buzzer
  unsigned long tiempoEliminado = 0;
  static const int MAX_TOMAS = 20;
  DateTime historialTomas[MAX_TOMAS];
  int totalTomas = 0;
  };

  // ---- Estructura de sincronización ----
  typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
  } HoraSync;

// ------------------- ESTRUCTURA DE MENSAJE -------------------
typedef struct {
  char nombre[20];
  char camilla[5];
  char medicamento[20];
  char dosis[5];
  char hora[6]; // HH:MM
} AlarmaPaciente;

  struct HistorialAlarma {
  String nombre;
  String medicamento;
  String dosis;
  String horaSilencio;        // hora programada
  String horaToma;            // hora de confirmación
  int idxPacienteOriginal;    // índice del paciente real en el arreglo
};

  DateTime convertirStringAHora(String horaStr) {
  int h = horaStr.substring(0, 2).toInt();   // Extrae horas
  int m = horaStr.substring(3, 5).toInt();   // Extrae minutos
  DateTime ahora = rtc.now();                 // Fecha actual
  return DateTime(ahora.year(), ahora.month(), ahora.day(), h, m, 0);
  }
  DateTime sumarHoras(DateTime hora, int horas) {
  uint32_t t = hora.unixtime() + horas * 3600; // suma horas en segundos
  return DateTime(t);

}


const int MAX_PACIENTES = 10;
Paciente pacientes[MAX_PACIENTES];
int totalPacientes = 0;

const int MAX_HISTORIAL = 20;
HistorialAlarma historial[MAX_HISTORIAL];
int historialCount = 0;
int idxHistorial = 0;

int pacienteHistSel = -1;
int pacienteActualEnPantalla = -1;

int paginaHistorial = 0;
const int alarmasPorPagina = 3;
int indicesBotonesEvento[alarmasPorPagina]; // índice real del historial en cada botón
int paginaDosis = 0;
const int dosisPorPagina = 5;

// ===================== COLA INVERSA (LIFO) - ÚNICO CAMBIO =====================
const int MAX_COLA = 10;
int colaAlarmas[MAX_COLA];
int tope = -1;  // -1 = vacía (COLA INVERSA)

// Control de timeout inteligente
struct ControlAlarma {
  unsigned long inicioAlarma = 0;
  bool buzzerActivo = false;
  static const unsigned long TIMEOUT_BUZZER = 60000; // 1 minuto
};
ControlAlarma controlAlarma;

// -------------------------- Pantallas --------------------------
enum Pantalla {
  PANT_MENU,
  PANT_VER_LISTA,
  PANT_VER_DETALLE,
  PANT_REG_SUBMENU,
  PANT_REG_SERIAL,
  PANT_CONFIG,
  PANT_CONFIG_HIST_LISTA,
  PANT_CONFIG_HIST_DOSIS,
  PANT_CONFIG_HIST_EVENTOS,
  PANT_CONFIG_RTC,
  PANT_CONFIG_FECHA,  
  PANT_BORRAR_ALARMA,
  PANT_CONFIRMAR_BORRAR,
  PANT_ALARMA 

};
Pantalla pantallaActual = PANT_MENU;

// ------------------------- Navegación Ver Pacientes -------------------------
int paginaActual = 0;
const int pacientesPorPagina = 3;
int pacienteSeleccionado = -1;

// ------------------------- Registro por Serial -------------------------
String campoActual = "nombre";
Paciente pacienteTemp;

// ------------------------- Reloj en pantalla -------------------------
String horaAnterior = "";
String fechaAnterior = "";
unsigned long ultimoUpdateHora = 0;
unsigned long ultimoRecordatorio = 0;

// ------------------------- Parpadeo alarma -------------------------
bool pantallaRoja = false;
unsigned long tiempoUltimoParpadeo = 0;

// ------------------------- Utilidades -------------------------
String getHoraString(const DateTime& dt) {
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
  return String(buf);
}
String getFechaString(const DateTime& dt) {
  char buf[11];
  sprintf(buf, "%02d/%02d/%04d", dt.day(), dt.month(), dt.year());
  return String(buf);
}
String formatearFechaHora(const DateTime& dt) {
  char buffer[25];
  sprintf(buffer, "%02d:%02d %02d/%02d/%04d", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
  return String(buffer);
}
void sumarHorasHHMM(String& hhmm, int horasASumar) {
  int hh = hhmm.substring(0,2).toInt();
  int mm = hhmm.substring(3,5).toInt();
  hh = (hh + (horasASumar % 24) + 24) % 24;
  char buf[6];
  sprintf(buf, "%02d:%02d", hh, mm);
  hhmm = String(buf);
}
bool coincideHHMMConAhora(const String& hhmm) {
  DateTime now = rtc.now();
  char buf[6];
  sprintf(buf, "%02d:%02d", now.hour(), now.minute());
  return (hhmm == String(buf));
}

// ===================== MANEJO DE COLA INVERSA =====================
bool colaVacia() {
  return tope == -1;
}

bool colaLlena() {
  return tope == MAX_COLA - 1;
}

void encolar(int idxPaciente) {
  if (!colaLlena()) {
    tope++;
    colaAlarmas[tope] = idxPaciente;
    pacientes[idxPaciente].alarmaActiva = true;
    pacientes[idxPaciente].silenciada = false;
    
    Serial.println("📥 Alarma encolada: " + pacientes[idxPaciente].nombre + 
                   " (Posición: " + String(tope) + ")");
  } else {
    Serial.println("⚠️ Cola llena: no se pudo encolar.");
  }
}

int desencolar() {
  if (colaVacia()) return -1;
  int idx = colaAlarmas[tope];
  tope--;
  return idx;
}

int verFrente() {
  if (!colaVacia()) {
    return colaAlarmas[tope];
  }
  return -1;
}

void interrumpirConNuevaAlarma(int idxNuevoPaciente) {
  Serial.println("🔀 INTERRUMPIENDO alarma actual por nueva prioridad");
  
  // Silenciar alarma actual si hay una
  if (!colaVacia()) {
    int idxActual = verFrente();
    if (idxActual >= 0) {
      pacientes[idxActual].silenciada = true;
      digitalWrite(BUZZER_PIN, LOW);
      Serial.println("🔇 Silenciando alarma actual: " + pacientes[idxActual].nombre);
    }
  }
  
  // Encobar nueva alarma (irá al tope)
  encolar(idxNuevoPaciente);
  
  // Reiniciar control de buzzer
  controlAlarma.inicioAlarma = millis();
  controlAlarma.buzzerActivo = true;
  
  // Mostrar inmediatamente la nueva alarma
  pacienteActualEnPantalla = idxNuevoPaciente;
  pantallaActual = PANT_ALARMA;
  mostrarPantallaAlarma(idxNuevoPaciente);
  
  // Activar buzzer
  digitalWrite(BUZZER_PIN, HIGH);
  pacientes[idxNuevoPaciente].ultimoBip = millis();
}

int colaLongitud() {
  return tope + 1;
}


// ------------------- ESP-NOW -------------------
void recibirConfirmacion(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  String msg = String((char*)incomingData);
  if (msg == "SILENCIAR") {
    Serial.println("🔇 Silenciar recibido desde esclavo");
    silenciarAlarmaActual();
}}

void enviarAlarma(AlarmaPaciente paciente) {
  esp_err_t result = esp_now_send(slaveAddress, (uint8_t *)&paciente, sizeof(paciente));
  if (result == ESP_OK) {
    Serial.println("✅ Alarma enviada al esclavo");
  } else {
    Serial.println("❌ Error enviando alarma al esclavo");
  }
}

void enviarMensaje(const char* msg) {
  esp_now_send(slaveAddress, (uint8_t*)msg, strlen(msg)+1);
}

// ---- Función para enviar hora/fecha ----
void enviarHoraSync() {
  DateTime now = rtc.now();

  HoraSync sync;
  sync.year   = now.year();
  sync.month  = now.month();
  sync.day    = now.day();
  sync.hour   = now.hour();
  sync.minute = now.minute();
  sync.second = now.second();

  esp_now_send(slaveAddress, (uint8_t*)&sync, sizeof(sync));
  Serial.println("⏰ Hora/Fecha enviada a esclavo");
}

// ------------------------- UI helpers -------------------------
void dibujarBoton(int x, int y, int w, int h, const char* texto) {
  tft.fillRect(x, y, w, h, TFT_GREEN);
  tft.drawRect(x, y, w, h, TFT_BLACK);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  int textoOffsetX = x + (w / 2) - (strlen(texto) * 6);
  int textoOffsetY = y + (h / 2) - 8;
  tft.setCursor(textoOffsetX, textoOffsetY);
  tft.print(texto);
}
void dibujarBotonVolver() {
  int x = 90, y = 400, w = 140, h = 50;
  tft.fillRect(x, y, w, h, TFT_GREEN);
  tft.drawRect(x, y, w, h, TFT_BLACK);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  const char* texto = "Volver";
  int textoOffsetX = x + (w / 2) - (strlen(texto) * 6);
  int textoOffsetY = y + (h / 2) - 8;
  tft.setCursor(textoOffsetX, textoOffsetY);
  tft.print(texto);
}
void actualizarHoraPantalla(const String& hora) {
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 80);
  tft.print("Hora: ");
  tft.print(hora);
}
void actualizarFechaPantalla(const String& fecha) {
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 105);
  tft.print("Fecha: ");
  tft.print(fecha);
}
void mostrarHoraDigital() {
  DateTime ahora = rtc.now();
  char bufferHora[9];
  sprintf(bufferHora, "%02d:%02d:%02d", ahora.hour(), ahora.minute(), ahora.second());
  tft.fillRect(200, 10, 110, 30, TFT_WHITE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(205, 15);
  tft.print(bufferHora);
}

// ------------------------- PANTALLA: MENÚ PRINCIPAL -------------------------
void mostrarMenuPrincipal() {
  pantallaActual = PANT_MENU;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(55, 30);
  tft.println("SISTEMA DE ALARMAS");

  DateTime now = rtc.now();
  horaAnterior = getHoraString(now);
  fechaAnterior = getFechaString(now);
  actualizarHoraPantalla(horaAnterior);
  actualizarFechaPantalla(fechaAnterior);

  int BTN_W = 215, BTN_H = 70, BTN_X = 55;
  dibujarBoton(BTN_X, 160, BTN_W, BTN_H, "Ver Pacientes");
  dibujarBoton(BTN_X, 250, BTN_W, BTN_H, "Nuevo Registro");
  dibujarBoton(BTN_X, 340, BTN_W, BTN_H, "Configuracion");

  Serial.println("--- MENU PRINCIPAL ---");
  Serial.println("1. Ver pacientes");
  Serial.println("2. Nuevo registro");
  Serial.println("3. Configuracion");
}

// ------------------------- PANTALLA: VER PACIENTES (LISTA) -------------------------
void interfazVerPacientes() {
  pantallaActual = PANT_VER_LISTA;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(50, 20);
  tft.println("LISTA DE PACIENTES");

  if (totalPacientes == 0) {
    // Mensaje si no hay pacientes
    tft.setCursor(40, 100);
    tft.println("Lista vacia");
    tft.setCursor(40, 130);
    tft.println("Use 'Nuevo Registro'");

    Serial.println("=== VER PACIENTES ===");
    Serial.println("No hay pacientes registrados. Use 'Nuevo Registro'.");

    dibujarBotonVolver();  // botón volver
    return;
  }

  int inicio = paginaActual * pacientesPorPagina;
  int fin = min(inicio + pacientesPorPagina, totalPacientes);

  for (int i = inicio; i < fin; i++) {
    int y = 60 + (i - inicio) * 100;

    tft.setCursor(10, y);
    tft.printf("%d. %s - %s", i + 1, pacientes[i].nombre.c_str(), pacientes[i].camilla.c_str());

    int botonVerX = 115;
    int botonVerY = y + 25;
    dibujarBoton(botonVerX, botonVerY, 100, 35, "Ver");
  }

  dibujarBoton(36, 340, 105, 45, "Anterior");
  dibujarBoton(180, 340, 110, 45, "Siguiente");
  dibujarBotonVolver();

  Serial.println("=== VER PACIENTES ===");
  Serial.println("Escribe 1..N para VER detalle");
  Serial.println("Escribe r1..rN para REGISTRAR toma");
  Serial.println("s = siguiente pagina | p = anterior | 0 = volver");
}

// ------------------------- PANTALLA: VER PACIENTES (DETALLE) -------------------------
void interfazDetallePaciente(int index) {
  pantallaActual = PANT_VER_DETALLE;
  Paciente& p = pacientes[index];
  DateTime proxima = (p.ultimaToma.year() == 2000) ? p.primeraDosis : p.ultimaToma + TimeSpan(0, p.frecuenciaHoras, 0, 0);

  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(50, 10); tft.println("FICHA DEL PACIENTE");

  tft.setCursor(10, 50);  tft.printf("Nombre: %s", p.nombre.c_str());
  tft.setCursor(10, 80);  tft.printf("Camilla: %s", p.camilla.c_str());
  tft.setCursor(10, 110); tft.printf("Medicamento: %s", p.medicamento.c_str());
  tft.setCursor(10, 140); tft.printf("Dosis: %s", p.dosis.c_str());
  tft.setCursor(10, 170); tft.printf("Cada %dh", p.frecuenciaHoras);

  // Primera dosis
  tft.setCursor(10, 200);
  tft.print("Primera dosis:");
  tft.setCursor(10, 230);
  tft.println(formatearFechaHora(p.primeraDosis));

  // Última dosis
  tft.setCursor(10, 260);
  tft.print("Ultima dosis:");
  tft.setCursor(10, 290);
  if (p.ultimaToma.year() == 2000) tft.println(""); // No hay dosis tomada aún
  else tft.println(formatearFechaHora(p.ultimaToma));

  // Próxima dosis
  tft.setCursor(10, 320);
  tft.print("Proxima dosis:");
  tft.setCursor(10, 350);
  tft.println(formatearFechaHora(proxima));
  pacienteTemp.primeraDosis = convertirStringAHora(pacienteTemp.horaInicio); // hora de la primera alarma
  pacienteTemp.ultimaToma = DateTime(2000, 1, 1, 0, 0, 0);                  // aún no se ha tomado

  dibujarBotonVolver();

  Serial.println("=== DETALLE ===");
  Serial.println("0 = volver a la lista");
}

// ------------------------- PANTALLA: NUEVO REGISTRO (SUBMENÚ) -------------------------
void interfazSubmenuRegistro() {
  pantallaActual = PANT_REG_SUBMENU;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(75, 20); tft.println("NUEVO REGISTRO");

  dibujarBoton(60, 80, 200, 50, "Programar alarma");
  dibujarBoton(60, 150, 200, 50, "Borrar alarma"); 
  dibujarBotonVolver();

  Serial.println("=== NUEVO REGISTRO ===");
  Serial.println("1. Programar alarma");
  Serial.println("2. Borrar alarma");
  Serial.println("0. Volver");
}

// ------------------------- PANTALLA: NUEVO REGISTRO (SERIAL) -------------------------
void iniciarRegistroPaciente() {
  campoActual = "nombre";
  pacienteTemp = Paciente();
  pantallaActual = PANT_REG_SERIAL;

  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(65, 20); tft.println("PROGRAMAR ALARMA");
  tft.setCursor(40, 60); tft.println("Ingrese nombre del");
  tft.setCursor(40, 90); tft.println("paciente a registrar:");

  dibujarBotonVolver();

  Serial.println("=== Programar alarma ===");
  Serial.println("(Escribe 'volver' para cancelar)");
  Serial.print("Nombre: ");
}

void mostrarAlarmasParaBorrar() {
  pantallaActual = PANT_BORRAR_ALARMA;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(80, 20);
  tft.println("BORRAR ALARMA");

  if (totalPacientes == 0) {
    // Mensaje si no hay alarmas
    tft.setCursor(40, 100); tft.println("No hay alarmas");
    tft.setCursor(40, 130);tft.println("registradas");
    dibujarBotonVolver();
    return;
  }

  int inicio = paginaActual * pacientesPorPagina;
  int fin = min(inicio + pacientesPorPagina, totalPacientes);

  for (int i = inicio; i < fin; i++) {
    int y = 60 + (i - inicio) * 100;

    // Mostrar solo el nombre (sin cama)
    tft.setCursor(10, y);
    tft.printf("%d. %s", i + 1, pacientes[i].nombre.c_str());

    // Botón BORRAR debajo
    int botonBorrarX = 115;
    int botonBorrarY = y + 25;
    dibujarBoton(botonBorrarX, botonBorrarY, 100, 35, "Borrar");
  }

  // Botones navegación
  dibujarBoton(36, 340, 105, 45, "Anterior");
  dibujarBoton(180, 340, 110, 45, "Siguiente");
  dibujarBotonVolver();

  Serial.println("=== BORRAR ALARMAS ===");
  Serial.println("Selecciona una alarma para borrar");
  Serial.println("s = siguiente pagina | p = anterior | 0 = volver");
}

void mostrarConfirmacionBorrar(int idx) {
  pantallaActual = PANT_CONFIRMAR_BORRAR;
  tft.fillScreen(TFT_WHITE);

  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);

  // Título
  tft.setCursor(55, 20);
  tft.println("CONFIRMAR BORRADO");

  // Mostrar datos del paciente
  tft.setCursor(20, 80);
  tft.printf("Paciente: %s", pacientes[idx].nombre.c_str());
  tft.setCursor(20, 110);
  tft.printf("Cama: %s", pacientes[idx].camilla.c_str());
  tft.setCursor(20, 140);
  tft.printf("Medicamento: %s", pacientes[idx].medicamento.c_str());
  
  // Aviso
  tft.setCursor(20, 180);
  tft.setTextColor(TFT_RED);
  tft.println("¿Seguro que desea borrar?");

  // Botones
  dibujarBoton(60, 250, 90, 50, "No");
  dibujarBoton(170, 250, 90, 50, "Si");

  Serial.println("Confirmar borrado del paciente:");
  Serial.println(pacientes[idx].nombre);
}

void actualizarPantallaRegistro() {
  tft.fillRect(0, 140, 320, 180, TFT_WHITE);

  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 140); tft.print("Nombre: "); tft.println(pacienteTemp.nombre);
  tft.setCursor(20, 170); tft.print("Camilla: "); tft.println(pacienteTemp.camilla);
  tft.setCursor(20, 200); tft.print("Med: "); tft.println(pacienteTemp.medicamento);
  tft.setCursor(20, 230); tft.print("Dosis: "); tft.println(pacienteTemp.dosis);

  tft.setCursor(20, 260); tft.print("Frec: ");
  if (campoActual == "frecuencia") tft.println("...esperando...");
  else { tft.print(pacienteTemp.frecuenciaHoras); tft.println(" h"); }

  // Fecha inicio - SIEMPRE mostrar
  tft.setCursor(20, 290); 
  tft.print("Fecha inicio: ");
  if (campoActual == "fechaInicio") {
    tft.println("DD/MM/AAAA");
  } else if (pacienteTemp.fechaInicioTratamiento.year() != 2000) {
    tft.println(getFechaString(pacienteTemp.fechaInicioTratamiento));
  } else {
    tft.println(); // línea vacía si no hay fecha
  }

  // Hora inicio - SIEMPRE mostrar (CORREGIDO)
  tft.setCursor(20, 320); 
  tft.print("Hora: ");
  if (campoActual == "horaInicio") {
    tft.println("HH:MM (24 Hrs)");
  } else if (!pacienteTemp.horaInicio.isEmpty()) {
    tft.println(pacienteTemp.horaInicio);
  } else {
    tft.println(); // línea vacía si no hay hora
  }
}

// ------------------------- PANTALLA: CONFIGURACIÓN -------------------------
void interfazConfiguracion() {
  pantallaActual = PANT_CONFIG;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(80, 20); tft.println("CONFIGURACION");

  dibujarBoton(60, 80, 200, 50, "Ver historial");
  dibujarBoton(60, 150, 200, 50, "Configurar hora");
  dibujarBoton(60, 220, 200, 50, "Configurar fecha");
  dibujarBotonVolver();

  Serial.println("=== CONFIGURACION ===");
  Serial.println("1. Ver historial");
  Serial.println("2. Configurar hora RTC (HH:MM)");
  Serial.println("3. Configurar fecha RTC (DD/MM/AAAA)"); 
  Serial.println("0. Volver");
}

// ------------------------- PANT: CONFIG -> RTC -------------------------
void configurarHoraRTC() {
  pantallaActual = PANT_CONFIG_RTC;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(40, 40);  tft.println("CONFIGURAR HORA RTC");
  tft.setCursor(40, 100); tft.println("Ingrese por Serial");
  tft.setCursor(40, 130); tft.println("formato HH:MM");
  dibujarBotonVolver();

  Serial.println("=== CONFIGURAR HORA RTC ===");
  Serial.println("Escribe la hora HH:MM y ENTER");
  Serial.println("0 para volver");
}

void configurarFechaRTC() {
  pantallaActual = PANT_CONFIG_FECHA;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(40, 40);  tft.println("CONFIGURAR FECHA RTC");
  tft.setCursor(40, 100); tft.println("Ingrese por Serial");
  tft.setCursor(40, 130); tft.println("formato DD/MM/AAAA");
  dibujarBotonVolver();

  Serial.println("=== CONFIGURAR FECHA RTC ===");
  Serial.println("Escribe la fecha DD/MM/AAAA y ENTER");
  Serial.println("0 para volver");
}

// ------------------------- PANT: CONFIG -> HISTORIAL (placeholder) -------------------------
// ------------------------- HISTORIAL REAL -------------------------

void mostrarHistorialPacientes() {
  pantallaActual = PANT_CONFIG_HIST_LISTA;
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(40, 20); 
  tft.println("HISTORIAL DE ALARMAS");

  if (historialCount == 0) {
    tft.setCursor(40, 100); tft.println("Aun no hay");
    tft.setCursor(40, 130); tft.println("alarmas registradas");
    
    dibujarBotonVolver();
    Serial.println("=== HISTORIAL VACIO ===");
    return;
  }

  int inicio = paginaHistorial * alarmasPorPagina;
  int fin = min(inicio + alarmasPorPagina, historialCount);

  for (int i = inicio; i < fin; i++) {
    int y = 60 + (i - inicio) * 80;   // 👈 ahora sí definimos y

    // Mostrar nombre del paciente/alarmas
    tft.setCursor(20, y);
    tft.printf("%d. %s", i + 1, historial[i].nombre.c_str());

    // Botón para ver evento
    int botonVerX = 115;
    int botonVerY = y + 25;
    dibujarBoton(botonVerX, botonVerY, 100, 35, "Evento");
    indicesBotonesEvento[i - inicio] = historial[i].idxPacienteOriginal; // guardamos el índice real
  }

  // Navegación
  dibujarBoton(36, 340, 105, 45, "Anterior");
  dibujarBoton(180, 340, 110, 45, "Siguiente");
  dibujarBotonVolver();

  // Debug por Serial
  Serial.println("=== HISTORIAL ===");
  for (int i = 0; i < historialCount; i++) {
    Serial.printf("[%d] %s\n", i + 1, historial[i].nombre.c_str());
  }
  Serial.println("Toca un boton 'Evento' para ver detalles");
}

void mostrarDosisHistorial(int idxPaciente) {
    pantallaActual = PANT_CONFIG_HIST_DOSIS;
    Paciente &p = pacientes[idxPaciente];

    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(2);

    // Encabezado
    tft.setCursor(20, 20);  
    tft.printf("Historial %s", p.nombre.c_str());
    tft.setCursor(20, 50);
    tft.printf("Camilla: %s", p.camilla.c_str());
    tft.setCursor(20, 80);
    tft.printf("Med: %s", p.medicamento.c_str());

    if (p.totalTomas == 0) {
        tft.setCursor(20, 120);
        tft.println("No hay dosis registradas");
        dibujarBotonVolver();
        return;
    }

    // Mostrar solo las últimas 5 dosis
    int inicio = max(0, p.totalTomas - 5);  
    int fin = p.totalTomas;

    int y = 120;
    for (int i = inicio; i < fin; i++) {
        DateTime dt = p.historialTomas[i];
        String fechaHora = String(dt.day()) + "/" + String(dt.month()) + " " +
                           String(dt.hour()) + ":" + (dt.minute() < 10 ? "0" : "") + String(dt.minute());
        tft.setCursor(20, y);
        tft.printf("%d) %s", i + 1, fechaHora.c_str());
        y += 30;
    }

    // Botón volver
    dibujarBotonVolver();
}

void mostrarEventosPaciente(int idxPaciente) {
    pantallaActual = PANT_CONFIG_HIST_EVENTOS;
    Paciente &p = pacientes[idxPaciente];

    tft.fillScreen(TFT_WHITE);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(2);

    // Encabezado
    tft.setCursor(20, 20);  
    tft.printf("Historial %s", p.nombre.c_str());
    tft.setCursor(20, 50);
    tft.printf("Camilla: %s", p.camilla.c_str());
    tft.setCursor(20, 80);
    tft.printf("Med: %s", p.medicamento.c_str());

    if (p.totalTomas == 0) {
        tft.setCursor(20, 120);
        tft.println("No hay dosis registradas");
        dibujarBotonVolver();
        return;
    }

    // Mostrar últimas 5 dosis
    int inicio = max(0, p.totalTomas - 5);
    int fin = p.totalTomas;

    int y = 120;
    for (int i = inicio; i < fin; i++) {
        DateTime dt = p.historialTomas[i];
        String fechaHora = String(dt.day()) + "/" + String(dt.month()) + " " +
                           String(dt.hour()) + ":" + (dt.minute() < 10 ? "0" : "") + String(dt.minute());
        tft.setCursor(20, y);
        tft.printf("%d) %s", i + 1, fechaHora.c_str());
        y += 30;
    }

    // Botón volver
    dibujarBotonVolver();

    // Debug
    Serial.printf("=== HISTORIAL DE %s ===\n", p.nombre.c_str());
    for (int i = inicio; i < fin; i++) {
        DateTime dt = p.historialTomas[i];
        Serial.printf("%d) %02d/%02d %02d:%02d\n", i+1, dt.day(), dt.month(), dt.hour(), dt.minute());
    }
}



// ------------------------- ALARMAS -------------------------
void bip() {
  digitalWrite(BUZZER_PIN, HIGH); delay(200);
  digitalWrite(BUZZER_PIN, LOW);  delay(200);
}

void mostrarPantallaAlarma(int idx) {
  DateTime ahora = rtc.now();
  String horaStr = getHoraString(ahora);

  // Fondo rojo
  tft.fillScreen(TFT_RED);

  // Texto de alarma
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(3);
  tft.setCursor(105, 40);
  tft.println("ALARMA");

  // Hora actual
  tft.setTextSize(2);
  tft.setCursor(20, 100);
  tft.println("Hora: " + horaStr);

  // Datos paciente
  tft.setCursor(20, 130);
  tft.println("Paciente: " + pacientes[idx].nombre);
  tft.setCursor(20, 160);
  tft.println("Camilla: " + pacientes[idx].camilla);   
  tft.setCursor(20, 190);
  tft.println("Med: " + pacientes[idx].medicamento);
  tft.setCursor(20, 220);
  tft.println("Dosis: " + pacientes[idx].dosis);

  // Estado del buzzer - VERSIÓN CORREGIDA
  tft.setCursor(20, 250);
  if (pacientes[idx].silenciada && controlAlarma.buzzerActivo) {
    // Silenciada manualmente durante el primer minuto
    tft.println("Estado: Silenciada");
  } else if (pacientes[idx].silenciada && !controlAlarma.buzzerActivo) {
    // Silenciada automáticamente (después de timeout)
    tft.println("Estado: Recordatorio");
  } else if (controlAlarma.buzzerActivo) {
    // Durante el primer minuto (sonando)
    unsigned long tiempoRestante = ControlAlarma::TIMEOUT_BUZZER - (millis() - controlAlarma.inicioAlarma);
    tft.printf("Estado: Sonando ", tiempoRestante / 1000);
  }
  // Botón Silenciar (izquierda)
  dibujarBoton(30, 325, 120, 70, "SILENCIAR");

  // Botón Confirmar (derecha)
  dibujarBoton(170, 325, 120, 70, "CONFIRMAR");

  // ⚡ Mostrar número de pendientes en cola
  int pendientes = colaLongitud();
  if (pendientes > 1) {
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setCursor(20, 280);
    tft.printf("Pendientes: %d", pendientes - 1);
  }
}
  void revisarAlarmas() {
  DateTime ahora = rtc.now();
  
  for (int i = 0; i < totalPacientes; i++) {
    // VERIFICAR SI EL TRATAMIENTO YA INICIÓ (fecha actual >= fecha inicio)
    bool tratamientoIniciado = (ahora.unixtime() >= pacientes[i].fechaInicioTratamiento.unixtime());
    
    // Activar en el minuto exacto (HH:MM) SOLO si el tratamiento ya inició
    if (!pacientes[i].alarmaActiva && 
        tratamientoIniciado && 
        coincideHHMMConAhora(pacientes[i].horaInicio)) {
      
      pacientes[i].alarmaActiva = true;
      pacientes[i].ultimoBip = millis();
      pacientes[i].silenciada = false;

      // Verificar si ya está en la cola
      bool yaEnCola = false;
      for (int j = 0; j <= tope; j++) {
        if (colaAlarmas[j] == i) {
          yaEnCola = true;
          break;
        }
      }
      
      if (!yaEnCola) {
        if (colaVacia()) {
          // Primera alarma - encolar normalmente
          encolar(i);
          controlAlarma.inicioAlarma = millis();
          controlAlarma.buzzerActivo = true;
        } else {
          // Interrumpir alarma actual con la nueva
          interrumpirConNuevaAlarma(i);
        }
      }
      
      // ==== ENVÍO DE DATOS AL ESCLAVO ==== (SOLO AQUÍ SE ENVÍA)
      AlarmaPaciente msg;
      strncpy(msg.nombre, pacientes[i].nombre.c_str(), sizeof(msg.nombre));
      strncpy(msg.camilla, pacientes[i].camilla.c_str(), sizeof(msg.camilla));
      strncpy(msg.medicamento, pacientes[i].medicamento.c_str(), sizeof(msg.medicamento));
      strncpy(msg.dosis, pacientes[i].dosis.c_str(), sizeof(msg.dosis));
      strncpy(msg.hora, pacientes[i].horaInicio.c_str(), sizeof(msg.hora));

      enviarAlarma(msg);  // Función que envía los datos al esclavo

      Serial.println("🚨 Alarma encolada: " + pacientes[i].nombre + " (" + pacientes[i].horaInicio + ")");
      Serial.println("   Fecha inicio: " + getFechaString(pacientes[i].fechaInicioTratamiento));
    }
  }
}

// ---------- SILENCIAR ALARMA ----------
void silenciarAlarmaActual() {
  if (pantallaActual == PANT_ALARMA && pacienteActualEnPantalla >= 0) {
    Paciente &p = pacientes[pacienteActualEnPantalla];

    if (p.alarmaActiva && !p.silenciada) {
      // 🔇 Apagar buzzer
      digitalWrite(BUZZER_PIN, LOW);
      noTone(BUZZER_PIN);

      // Marcar que está silenciada pero sigue activa
      p.silenciada = true;

      Serial.println("🔕 Alarma silenciada para: " + p.nombre);
      SerialBT.println("🔕 Alarma silenciada para: " + p.nombre);

      // Mantener en pantalla la alarma
      mostrarPantallaAlarma(pacienteActualEnPantalla);

    }
  }
}

// ---------- CONFIRMAR ALARMA ----------
void confirmarAlarma() {
  DateTime ahora = rtc.now();

  // Confirmamos el paciente que está al frente de la cola
  int idx = verFrente();
  if (idx == -1) {
    Serial.println("⚠️ No hay alarmas en cola para confirmar.");
    SerialBT.println("⚠️ No hay alarmas en cola para confirmar.");
    return;
  }

  Paciente &p = pacientes[idx];

  // ✅ Confirmar alarma
  p.alarmaActiva = false;
  p.silenciada = false;
  digitalWrite(BUZZER_PIN, LOW);
  noTone(BUZZER_PIN);
  p.ultimaToma = ahora;

  // NUEVO (calcula desde la hora real de confirmación):
  DateTime proximaAlarma = ahora + TimeSpan(0, p.frecuenciaHoras, 0, 0);
  char buffer[6];
  sprintf(buffer, "%02d:%02d", proximaAlarma.hour(), proximaAlarma.minute());
  p.horaInicio = String(buffer);
  p.proxima = proximaAlarma;  // Mantener consistencia

  // Guardar en historial del paciente
  if (p.totalTomas < Paciente::MAX_TOMAS) {
    p.historialTomas[p.totalTomas++] = ahora;
  } else {
    for (int j = 1; j < Paciente::MAX_TOMAS; j++) {
      p.historialTomas[j-1] = p.historialTomas[j];
    }
    p.historialTomas[Paciente::MAX_TOMAS - 1] = ahora;
  }

  Serial.println("✅ Alarma confirmada: " + p.nombre + " | prox: " + p.horaInicio);
  SerialBT.println("✅ Alarma confirmada: " + p.nombre);

  // Actualizar historial global
  int idxHist = -1;
  for (int h = 0; h < historialCount; h++) {
    if (historial[h].idxPacienteOriginal == idx) {
      idxHist = h;
      break;
    }
  }
  if (idxHist == -1 && historialCount < MAX_HISTORIAL) {
    historial[historialCount].nombre = p.nombre;
    historial[historialCount].medicamento = p.medicamento;
    historial[historialCount].dosis = p.dosis;
    historial[historialCount].horaSilencio = p.horaInicio;
    historial[historialCount].horaToma = getHoraString(ahora);
    historial[historialCount].idxPacienteOriginal = idx;
    historialCount++;
  } else if (idxHist != -1) {
    historial[idxHist].horaToma = getHoraString(ahora);
  }

  // Quitar del frente de la cola
  desencolar();

  // ✅ SOLO ENVIAR "CONFIRMADA", NO REENVIAR LA ALARMA
  enviarMensaje("CONFIRMADA");

  // 🔄 RESTAURAR ALARMA ANTERIOR SI EXISTE
  if (!colaVacia()) {
    int siguiente = verFrente();
    pacienteActualEnPantalla = siguiente;
    
    // Reactivar la alarma anterior
    pacientes[siguiente].silenciada = false;
    controlAlarma.inicioAlarma = millis();
    controlAlarma.buzzerActivo = true;
    
    mostrarPantallaAlarma(siguiente);
    
    // Reactivar buzzer
    digitalWrite(BUZZER_PIN, HIGH);
    pacientes[siguiente].ultimoBip = millis();
    
    Serial.println("🔄 Restaurando alarma anterior: " + pacientes[siguiente].nombre);
  } else {
    // ⚡ Si no hay más alarmas → ir directo al menú
    pacienteActualEnPantalla = -1;
    controlAlarma.buzzerActivo = false;
    pantallaActual = PANT_MENU;
    mostrarMenuPrincipal();
  }
}

void leerTouchMenu() {
  if (ts.touched()) {
    touch = ts.getPoint();
    int x = map(touch.y, Y_MIN, Y_MAX, 0, tft.width());
    int y = map(touch.x, X_MIN, X_MAX, 0, tft.height());

    // --- MENÚ PRINCIPAL ---
    if (pantallaActual == PANT_MENU) {
      if (x >= 55 && x <= 270 && y >= 160 && y <= 230) interfazVerPacientes();
      else if (x >= 55 && x <= 270 && y >= 250 && y <= 320) interfazSubmenuRegistro();
      else if (x >= 55 && x <= 270 && y >= 340 && y <= 410) interfazConfiguracion();
    }

    // --- VER PACIENTES: LISTA ---
    else if (pantallaActual == PANT_VER_LISTA) {
      // Botón Anterior
      if (x >= 36 && x <= 36+105 && y >= 340 && y <= 340+45) {
        if ((paginaActual+1)*pacientesPorPagina < totalPacientes) paginaActual++;
        interfazVerPacientes();
      }
      // Botón Siguiente
      else if (x >= 180 && x <= 180+110 && y >= 340 && y <= 340+45) {
        if (paginaActual > 0) paginaActual--;
        interfazVerPacientes();
      }
      // Botón Volver
      else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) mostrarMenuPrincipal();

      // Botones "Ver" de cada paciente
      int inicio = paginaActual * pacientesPorPagina;
      for (int i = inicio; i < min(inicio + pacientesPorPagina, totalPacientes); i++) {
        int yBtn = 60 + (i - inicio) * 100 + 25; // y del botón "Ver"
        if (x >= 115 && x <= 215 && y >= yBtn && y <= yBtn + 35) {
          pacienteSeleccionado = i;
          interfazDetallePaciente(i);
        }
      }
    }

    // --- DETALLE PACIENTE ---
    else if (pantallaActual == PANT_VER_DETALLE) {
      if (x >= 90 && x <= 230 && y >= 400 && y <= 450) interfazVerPacientes();
    }

    // --- NUEVO REGISTRO SUBMENU ---
    else if (pantallaActual == PANT_REG_SUBMENU) {
      if (x >= 60 && x <= 260 && y >= 80 && y <= 130) iniciarRegistroPaciente();
      else if (x >= 60 && x <= 260 && y >= 150 && y <= 200) mostrarAlarmasParaBorrar();
      else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) mostrarMenuPrincipal();
    }

    // --- BORRAR ALARMAS: LISTA ---
    else if (pantallaActual == PANT_BORRAR_ALARMA) {
      // Botón Anterior
      if (x >= 36 && x <= 36+105 && y >= 340 && y <= 340+45) {
        if ((paginaActual+1)*pacientesPorPagina < totalPacientes) paginaActual++;
        mostrarAlarmasParaBorrar();
      }
      // Botón Siguiente
      else if (x >= 180 && x <= 180+110 && y >= 340 && y <= 340+45) {
        if (paginaActual > 0) paginaActual--;
        mostrarAlarmasParaBorrar();
      }
      // Botón Volver
      else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) {
        interfazSubmenuRegistro();
      }

      // Botones "Borrar" de cada paciente
      int inicio = paginaActual * pacientesPorPagina;
      for (int i = inicio; i < min(inicio + pacientesPorPagina, totalPacientes); i++) {
        int yBtn = 60 + (i - inicio) * 100 + 25; // misma lógica que en Ver
        if (x >= 115 && x <= 215 && y >= yBtn && y <= yBtn + 35) {
          pacienteSeleccionado = i;
          mostrarConfirmacionBorrar(i); // abre confirmación
        }
      }
    }

    // --- CONFIRMAR BORRAR ---
    else if (pantallaActual == PANT_CONFIRMAR_BORRAR) {
      // Botón Sí
      if (x >= 60 && x <= 150 && y >= 250 && y <= 300) {
        // Eliminar paciente
        for (int i = pacienteSeleccionado; i < totalPacientes - 1; i++) {
          pacientes[i] = pacientes[i + 1];
        }
        totalPacientes--;

        Serial.println("✅ Paciente eliminado.");
        mostrarAlarmasParaBorrar(); // volver a la lista
      }
      // Botón No
      else if (x >= 170 && x <= 260 && y >= 250 && y <= 300) {
        mostrarAlarmasParaBorrar(); // volver sin borrar
      }
    }

    // --- CONFIGURACIÓN ---
    else if (pantallaActual == PANT_CONFIG) {
      if (x >= 60 && x <= 260 && y >= 80 && y <= 130) mostrarHistorialPacientes();
      else if (x >= 60 && x <= 260 && y >= 150 && y <= 200) configurarHoraRTC();
      else if (x >= 60 && x <= 260 && y >= 220 && y <= 270) configurarFechaRTC();
      else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) mostrarMenuPrincipal();
    }

    // --- HISTORIAL: LISTA ---
    else if (pantallaActual == PANT_CONFIG_HIST_LISTA) {
      // Botón Anterior
      if (x >= 180 && x <= 180+110 && y >= 340 && y <= 340+45) {
        if (paginaHistorial > 0) {
          paginaHistorial--;
          mostrarHistorialPacientes();
        }
      }
      // Botón Siguiente
      else if (x >= 36 && x <= 36+105 && y >= 340 && y <= 340+45) {
        int maxPag = (historialCount - 1) / alarmasPorPagina;
        if (paginaHistorial < maxPag) {
          paginaHistorial++;
          mostrarHistorialPacientes();
        }
      }
      // Botón Volver
      else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) {
        pantallaActual = PANT_CONFIG;
        interfazConfiguracion();
      }

      // Botones "Evento"
      int inicio = paginaHistorial * alarmasPorPagina;
      for (int i = inicio; i < min(inicio + alarmasPorPagina, historialCount); i++) {
        int yBtn = 60 + (i - inicio) * 80 + 25;  // misma fórmula que en mostrarHistorialPacientes
        if (x >= 115 && x <= 215 && y >= yBtn && y <= yBtn + 35) {
            pacienteHistSel = indicesBotonesEvento[i - inicio]; // ⬅️ paciente correcto
            mostrarEventosPaciente(pacienteHistSel);  
        }
      }
    }

    // --- HISTORIAL: DETALLE ---
    else if (pantallaActual == PANT_CONFIG_HIST_EVENTOS) {
      // Botón Volver
      if (x >= 90 && x <= 230 && y >= 400 && y <= 450) {
        mostrarHistorialPacientes(); // regresa a la lista
      }
    }

    // --- PANTALLA DE ALARMA ---
    else if (pantallaActual == PANT_ALARMA) {
      // CONFIRMAR
      if (x >= 30 && x <= 150 && y >= 325 && y <= 395) {
        confirmarAlarma();
        // NO enviarMensaje("CONFIRMADA") aquí - ya se envía en confirmarAlarma()
      }
      // SILENCIAR
      else if (x >= 170 && x <= 290 && y >= 325 && y <= 395) {
        silenciarAlarmaActual();
        enviarMensaje("SILENCIAR");
      }
    }

    // --- BOTON VOLVER GENERAL ---
    else if (x >= 90 && x <= 230 && y >= 400 && y <= 450) mostrarMenuPrincipal();
  }
}

void setup() {

  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_CONFIRMAR, INPUT_PULLUP);
  
  Serial.begin(115200);
  SerialBT.begin("ESP32_ALARMAS");  
  tft.init(); tft.setRotation(2); tft.fillScreen(TFT_WHITE);
  ts.begin(); ts.setRotation(1);  // rotación según tu pantalla y TFT

  Wire.begin(25, 14);
  if (!rtc.begin()) {
    tft.setCursor(10, 10);
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.println("No se detecta el RTC");
    while (true);
  }

  totalPacientes = 0;
  DateTime ahora = rtc.now();

  // =================== INICIO ESP-NOW ===================
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inicializando ESP-NOW");
    while(true);
  }

  // Registrar callback para recibir confirmación del esclavo
  esp_now_register_recv_cb(recibirConfirmacion);

  // Agregar esclavo como peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, slaveAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("No se pudo agregar esclavo como peer");
  }

  Serial.println("Maestro listo!");
  // =================== FIN ESP-NOW ===================

  mostrarMenuPrincipal();
}

// ============================================================================================================
// LOOP
// ============================================================================================================
void loop() {
  static unsigned long lastSync = 0;
  static unsigned long ultimoRecordatorio = 0;

  if (millis() - lastSync > 60000) {   // cada 60s
    enviarHoraSync();
    lastSync = millis();
  }

  // Reloj en pantalla
  if (pantallaActual == PANT_MENU) {  // solo en menú principal
      if (millis() - ultimoUpdateHora >= 500) {
          ultimoUpdateHora = millis();
          DateTime now = rtc.now();
          String h = getHoraString(now), f = getFechaString(now);
          if (h != horaAnterior) { horaAnterior = h; actualizarHoraPantalla(h); }
          if (f != fechaAnterior) { fechaAnterior = f; actualizarFechaPantalla(f); }
       }
  }

  // Alarmas / botón físico
  revisarAlarmas();
// =================== CONTROL DE TIMEOUT INTELIGENTE ===================
if (controlAlarma.buzzerActivo && !colaVacia()) {
  int idxActual = verFrente();
  
  if (millis() - controlAlarma.inicioAlarma > ControlAlarma::TIMEOUT_BUZZER) {
    // ⏰ TIMEOUT: Silenciar buzzer pero mantener alarma en pantalla
    digitalWrite(BUZZER_PIN, LOW);
    controlAlarma.buzzerActivo = false;
    pacientes[idxActual].silenciada = true;
    
    Serial.println("🔇 Timeout de buzzer para: " + pacientes[idxActual].nombre);
    
    // Actualizar pantalla inmediatamente para mostrar nuevo estado
    if (pantallaActual == PANT_ALARMA && pacienteActualEnPantalla == idxActual) {
      mostrarPantallaAlarma(idxActual);
    }
  } else if (!pacientes[idxActual].silenciada) {
    // Parpadeo normal del buzzer (durante el primer minuto) - 3 SEGUNDOS
    if (millis() - pacientes[idxActual].ultimoBip > 3000) {
      digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
      pacientes[idxActual].ultimoBip = millis();
    }
  }
}

// =================== RECORDATORIOS CADA 30 SEGUNDOS ===================
if (!colaVacia()) {
  int idxActual = verFrente();
  
  // Recordatorios independientes - funcionan SIEMPRE que haya alarma activa
  if (millis() - ultimoRecordatorio > 30000) { // Cada 30 segundos
    // Solo hacer recordatorio si la alarma está activa (no confirmada)
    if (pacientes[idxActual].alarmaActiva) {
      // 2 pitidos de recordatorio
      for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
      }
      ultimoRecordatorio = millis();
      Serial.println("🔔 Recordatorio para: " + pacientes[idxActual].nombre);
    }
  }
}

// Mostrar siguiente paciente en alarma si estamos en pantallas principales
if (pantallaActual == PANT_MENU || pantallaActual == PANT_VER_LISTA || pantallaActual == PANT_CONFIG) {
  if (!colaVacia()) {
    int idx = verFrente();   // paciente al frente de la cola
    pacienteActualEnPantalla = idx;

    pantallaActual = PANT_ALARMA;
    mostrarPantallaAlarma(idx);

    pacientes[idx].ultimoBip = millis();
    if (!pacientes[idx].silenciada && controlAlarma.buzzerActivo) {
      digitalWrite(BUZZER_PIN, HIGH);
    }
  }
}

if (pantallaActual == PANT_ALARMA && pacienteActualEnPantalla >= 0) {
  Paciente &p = pacientes[pacienteActualEnPantalla];
  if (p.alarmaActiva && !p.silenciada && controlAlarma.buzzerActivo){
    unsigned long tiempoActual = millis();
    unsigned long ciclo = (tiempoActual - p.ultimoBip) % 2000; // Ciclo de 3 segundos
    
    if (ciclo < 100) {
      // Primer BIP - primeros 100ms
      digitalWrite(BUZZER_PIN, HIGH);
    } else if (ciclo < 200) {
      // Pausa entre BIPs
      digitalWrite(BUZZER_PIN, LOW);
    } else if (ciclo < 300) {
      // Segundo BIP
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      // Silencio hasta completar 3 segundos
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    // Si no está activo, apagar buzzer
    digitalWrite(BUZZER_PIN, LOW);
  }
}

  leerTouchMenu();

  // Navegación por Serial
  if (SerialBT.available()) {
    String in = SerialBT.readStringUntil('\n'); in.trim();
    
    // Si llega la orden de CONFIRMAR (o la palabra que definas), siempre tiene prioridad
    if (in.length() > 0 && in.equalsIgnoreCase(BT_CONFIRM_CMD)) {
      confirmarAlarma();
      SerialBT.println("✅ Alarma confirmada por Bluetooth"); // procesada la orden; esperar siguiente input
    }

    // ---------- MENÚ ----------
    if (pantallaActual == PANT_MENU) {
      if (in == "1") interfazVerPacientes();
      else if (in == "2") interfazSubmenuRegistro();
      else if (in == "3") interfazConfiguracion();
    }

    // ---------- VER PACIENTES: LISTA ----------
    else if (pantallaActual == PANT_VER_LISTA) {
      if (in == "0") { mostrarMenuPrincipal(); }
      else if (in == "n") { if ((paginaActual + 1) * pacientesPorPagina < totalPacientes) paginaActual++; interfazVerPacientes(); }
      else if (in == "p") { if (paginaActual > 0) paginaActual--; interfazVerPacientes(); }
      else {
        int sel = in.toInt() - 1;
        if (sel >= 0 && sel < totalPacientes) {
          pacienteSeleccionado = sel;
          interfazDetallePaciente(sel);
        } else Serial.println("Seleccion invalida.");
      }
    }

    // ---------- VER PACIENTES: DETALLE ----------
    else if (pantallaActual == PANT_VER_DETALLE) {
      if (in == "0") interfazVerPacientes();
    }

    // ---------- NUEVO REGISTRO: SUBMENÚ ----------
    else if (pantallaActual == PANT_REG_SUBMENU) {
      if (in == "1") iniciarRegistroPaciente();
      else if (in == "2") mostrarAlarmasParaBorrar(); 
      else if (in == "0") mostrarMenuPrincipal();
    }

    else if (pantallaActual == PANT_BORRAR_ALARMA) {
      if (in == "0") {
        interfazSubmenuRegistro();
      } else {
        int idx = in.toInt() - 1;
        if (idx >= 0 && idx < totalPacientes) {
          pacienteSeleccionado = idx;
          mostrarConfirmacionBorrar(idx);
        } else {
          Serial.println("Seleccion invalida.");
        }
      }
    }

    else if (pantallaActual == PANT_CONFIRMAR_BORRAR) {
      if (in == "1") {
        // Eliminar paciente/alarma
        for (int i = pacienteSeleccionado; i < totalPacientes - 1; i++) {
          pacientes[i] = pacientes[i + 1];
        }
        totalPacientes--;

        Serial.println("✅ Alarma eliminada.");
        SerialBT.println("✅ Alarma eliminada.");

        mostrarMenuPrincipal();
      }
      else if (in == "2" || in == "0") {
        mostrarMenuPrincipal();
      }
    }

// ---------- NUEVO REGISTRO: SERIAL paso a paso ----------
else if (pantallaActual == PANT_REG_SERIAL) {
  if (in.equalsIgnoreCase("volver") || in == "0") {
    Serial.println("↩️ Registro cancelado.");
    interfazSubmenuRegistro();
  } else if (campoActual == "nombre") {
    pacienteTemp.nombre = in; 
    campoActual = "camilla"; 
    actualizarPantallaRegistro(); 
    SerialBT.print("Camilla: ");
  } else if (campoActual == "camilla") {
    pacienteTemp.camilla = in; 
    campoActual = "medicamento"; 
    actualizarPantallaRegistro(); 
    SerialBT.print("Medicamento: ");
  } else if (campoActual == "medicamento") {
    pacienteTemp.medicamento = in; 
    campoActual = "dosis"; 
    actualizarPantallaRegistro(); 
    SerialBT.print("Dosis: ");
  } else if (campoActual == "dosis") {
    pacienteTemp.dosis = in; 
    campoActual = "frecuencia"; 
    actualizarPantallaRegistro(); 
    SerialBT.print("Frecuencia (h): ");
  } else if (campoActual == "frecuencia") {
    pacienteTemp.frecuenciaHoras = in.toInt(); 
    campoActual = "fechaInicio";  // 👈 CAMBIÉ: Ahora va primero fecha
    actualizarPantallaRegistro(); 
    SerialBT.print("Fecha inicio (DD/MM/AAAA): ");
  }
  
  // NUEVO ORDEN: Primero fecha
  else if (campoActual == "fechaInicio") {
    if (in.length() == 10 && in.charAt(2) == '/' && in.charAt(5) == '/') {
      int dd = in.substring(0,2).toInt();
      int mm = in.substring(3,5).toInt();
      int aa = in.substring(6,10).toInt();
      
      // Guardar solo la fecha por ahora (hora temporal)
      pacienteTemp.fechaInicioTratamiento = DateTime(aa, mm, dd, 0, 0, 0);
      campoActual = "horaInicio";  // 👈 Luego pedimos hora
      actualizarPantallaRegistro();
      SerialBT.print("Hora inicio (HH:MM): ");
    } else {
      SerialBT.println("Formato inválido. Usa DD/MM/AAAA");
    }
  }
  
  // LUEGO: Hora
  else if (campoActual == "horaInicio") {
    pacienteTemp.horaInicio = in;
    
    // Combinar con la fecha que ya capturamos
    int hh = pacienteTemp.horaInicio.substring(0,2).toInt();
    int min = pacienteTemp.horaInicio.substring(3,5).toInt();
    
    // Actualizar fechaInicioTratamiento con la hora correcta
    pacienteTemp.fechaInicioTratamiento = DateTime(
      pacienteTemp.fechaInicioTratamiento.year(),
      pacienteTemp.fechaInicioTratamiento.month(), 
      pacienteTemp.fechaInicioTratamiento.day(),
      hh, min, 0
    );
    
    pacienteTemp.primeraDosis = pacienteTemp.fechaInicioTratamiento;
    pacienteTemp.ultimaToma = DateTime(2000, 1, 1, 0, 0, 0);
    pacienteTemp.proxima = sumarHoras(pacienteTemp.primeraDosis, pacienteTemp.frecuenciaHoras);
    pacienteTemp.alarmaActiva = false;
    
    if (totalPacientes < MAX_PACIENTES) {
      pacientes[totalPacientes++] = pacienteTemp;
      SerialBT.println("✅ Paciente registrado para: " + getFechaString(pacienteTemp.fechaInicioTratamiento));
    } else {
      SerialBT.println("❌ Lista de pacientes llena.");
    }
    
    mostrarMenuPrincipal();
  }
}

    // ---------- CONFIGURACION ----------
    else if (pantallaActual == PANT_CONFIG) {
      if (in == "1") mostrarHistorialPacientes();
      else if (in == "2") configurarHoraRTC();
      else if (in == "3") configurarFechaRTC();  
      else if (in == "0") mostrarMenuPrincipal();
    }

    // ---------- CONFIG -> HISTORIAL LISTA ----------
    else if (pantallaActual == PANT_CONFIG_HIST_LISTA) {
      if (in == "0") interfazConfiguracion();
      else {
        int sel = in.toInt();
        if (sel >= 1 && sel <= 3) { 
          pacienteHistSel = sel - 1; 
          mostrarEventosPaciente(pacienteHistSel);  // ✅ corregido
        }
        else Serial.println("Seleccion invalida.");
      }
    }

    // ---------- CONFIG -> HISTORIAL EVENTOS ----------
    else if (pantallaActual == PANT_CONFIG_HIST_EVENTOS) {  // ✅ corregido
      if (in == "0") mostrarHistorialPacientes();
    }

    else if (pantallaActual == PANT_CONFIG_HIST_DOSIS) {
      if (in == "0") {
        pantallaActual = PANT_CONFIG_HIST_EVENTOS;  // ✅ corregido
        mostrarEventosPaciente(pacienteActualEnPantalla);  // ✅ corregido
      }
    }

    // ---------- CONFIG -> RTC ----------
    else if (pantallaActual == PANT_CONFIG_RTC) {
      if (in == "0") interfazConfiguracion();
      else if (in.length() == 5 && in.charAt(2) == ':') {
        int hh = in.substring(0,2).toInt();
        int mm = in.substring(3,5).toInt();
        DateTime now = rtc.now();
        rtc.adjust(DateTime(now.year(), now.month(), now.day(), hh, mm, 0));
        Serial.println("[OK] Hora RTC ajustada.");
        interfazConfiguracion();
      } else {
        Serial.println("Formato invalido. Usa HH:MM o 0 para volver.");
        SerialBT.println("Formato invalido. Usa HH:MM o 0 para volver.");
      }
    }

    // ---------- CONFIG -> FECHA ----------
    else if (pantallaActual == PANT_CONFIG_FECHA) {
      if (in == "0") interfazConfiguracion();
      else if (in.length() == 10 && in.charAt(2) == '/' && in.charAt(5) == '/') {
        int dd = in.substring(0,2).toInt();
        int mm = in.substring(3,5).toInt();
        int aa = in.substring(6,10).toInt();
        DateTime now = rtc.now();
        rtc.adjust(DateTime(aa, mm, dd, now.hour(), now.minute(), now.second()));
        Serial.println("[OK] Fecha RTC ajustada.");
        interfazConfiguracion();
      } else {
        Serial.println("Formato invalido. Usa DD/MM/AAAA o 0 para volver.");
      }
    }
    
    if (digitalRead(BTN_CONFIRMAR) == LOW) {
      Serial.println("BOTON CONFIRMAR PRESIONADO (en proyecto grande)");
      delay(200); // anti-rebote básico
    }
  }
}
