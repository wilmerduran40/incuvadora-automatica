/*
 * Incubadora Automatica v2.0 - ESP32
 * 
 * Componentes:
 *  - ESP32
 *  - Sensor DHT22 (AM2302)
 *  - Modulo Relay 3 canales (calefactor, humificador, motor rodillos)
 *  - Humificador ultrasonico
 *  - Motor rodillos 110V para volteo automatico
 *  - Pantalla TFT ST7789 (170x320 IPS)
 *
 * Librerias requeridas:
 *  - AM2302-Sensor
 *  - Arduino_GFX
 *
 * Fases incubacion pollos:
 *  - Dias 1-17:  Desarrollo  (Humedad 50-55%, Volteo ON cada 2h)
 *  - Dias 18-21: Lockdown    (Humedad 65-70%, Volteo OFF)
 *
 * Proteccion contra cortes electricos: Preferences (NVS flash)
 * Comandos: +/-:temp h:hum t:vol s:save d:info r:reset c:calefactor
 */

#include <AM2302-Sensor.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>

// ===================== PINES =====================
constexpr unsigned int PIN_DHT22       = 4;
constexpr unsigned int PIN_RELAY_HEAT  = 25;
constexpr unsigned int PIN_RELAY_HUM   = 26;
constexpr unsigned int PIN_RELAY_MOTOR = 27;

// ===================== OBJETOS =====================
AM2302::AM2302_Sensor am2302(PIN_DHT22);
Arduino_DataBus *bus = new Arduino_ESP32SPI(16, 5, 18, 23);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 17, 0, true, 170, 320, 35, 0, 35, 0);
Preferences preferences;

// ===================== COLORES =====================
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GREEN   0x07E0
#define COLOR_RED     0xF800
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_ORANGE  0xFDA0
#define COLOR_DKGREY  0x4208
#define COLOR_BLUE    0x001F
#define COLOR_MAGENTA 0xF81F

// ===================== CONSTANTES =====================
const unsigned long INTERVALO_VOLTEO    = 2UL * 60UL * 60UL * 1000UL;
const unsigned long DURACION_VOLTEO     = 10000UL;
const unsigned long INTERVALO_LECTURA   = 2000UL;
const unsigned long INTERVALO_PANTALLA  = 1000UL;
const unsigned long INTERVALO_GUARDADO  = 60000UL;
const int DIAS_INCUBACION              = 21;
const int DIA_LOCKDOWN                  = 18;

// Humedad fase desarrollo (dias 1-17)
const float HUM_MIN_DESARROLLO = 50.0;
const float HUM_MAX_DESARROLLO = 55.0;

// Humedad fase lockdown (dias 18-21)
const float HUM_MIN_LOCKDOWN = 65.0;
const float HUM_MAX_LOCKDOWN = 70.0;

// Temperatura constante todo el ciclo
float tempObjetivo = 37.5;
const float TEMP_MIN      = 37.2;
const float TEMP_MAX      = 37.8;

// ===================== VARIABLES DE ESTADO =====================
float temperatura = 0.0;
float humedad = 0.0;

bool estadoCalefactor = false;
bool estadoHumificador = false;
bool motorVolteando = false;
bool enLockdown = false;

unsigned long ultimoVolteo = 0;
unsigned long inicioVolteo = 0;
unsigned long ultimaLectura = 0;
unsigned long ultimaPantalla = 0;
unsigned long ultimaGuardado = 0;
unsigned long inicioIncubacion = 0;
unsigned long uptimeAcumulado = 0;
unsigned long millisEnUltimoGuardado = 0;

int diaActual = 0;

bool manualCal = false;
bool manualHum = false;

// ===================== PERSISTENCIA (PROTECCION CORTE) =====================

void guardarEstado() {
  preferences.begin("incubadora", false);

  unsigned long uptimeTotal = uptimeAcumulado + millis();
  preferences.putULong("uptime", uptimeTotal);
  preferences.putULong("ultimoVol", ultimoVolteo);
  preferences.putBool("manCal", manualCal);
  preferences.putBool("manHum", manualHum);

  preferences.end();
  Serial.println(F("Estado guardado."));
}

void cargarEstado() {
  preferences.begin("incubadora", true);

  unsigned long uptimeGuardado = preferences.getULong("uptime", 0);
  ultimoVolteo = preferences.getULong("ultimoVol", 0);
  manualCal = preferences.getBool("manCal", false);
  manualHum = preferences.getBool("manHum", false);

  preferences.end();

  uptimeAcumulado = uptimeGuardado;
  millisEnUltimoGuardado = millis();
}

void borrarEstado() {
  preferences.begin("incubadora", false);
  preferences.clear();
  preferences.end();

  uptimeAcumulado = 0;
  inicioIncubacion = millis();
  ultimoVolteo = millis();
  millisEnUltimoGuardado = millis();
  manualCal = false;
  manualHum = false;

  Serial.println(F("Estado borrado. Reiniciando."));
}

// ===================== LOGICA DE FASES =====================

unsigned long obtenerUptimeTotal() {
  return uptimeAcumulado + millis();
}

void calcularDia() {
  unsigned long uptimeTotal = obtenerUptimeTotal();
  diaActual = (int)(uptimeTotal / 86400000UL) + 1;
  if (diaActual > DIAS_INCUBACION) {
    diaActual = DIAS_INCUBACION;
  }
}

void verificarFase() {
  bool eraLockdown = enLockdown;
  enLockdown = (diaActual >= DIA_LOCKDOWN);

  if (enLockdown && !eraLockdown) {
    Serial.println(F("*** LOCKDOWN - Dia 18+ ***"));
    Serial.println(F("Volteo DETENIDO. Humedad: 65-70%"));
    if (motorVolteando) {
      detenerVolteo();
    }
  }
}

float obtenerHumMin() {
  return enLockdown ? HUM_MIN_LOCKDOWN : HUM_MIN_DESARROLLO;
}

float obtenerHumMax() {
  return enLockdown ? HUM_MAX_LOCKDOWN : HUM_MAX_DESARROLLO;
}

// ===================== CONTROL DE ACTUADORES =====================

void controlarCalefactor() {
  if (manualCal) return;

  if (temperatura < TEMP_MIN && !estadoCalefactor) {
    digitalWrite(PIN_RELAY_HEAT, LOW);
    estadoCalefactor = true;
  } else if (temperatura > TEMP_MAX && estadoCalefactor) {
    digitalWrite(PIN_RELAY_HEAT, HIGH);
    estadoCalefactor = false;
  }
}

void controlarHumificador() {
  float humMin = obtenerHumMin();
  float humMax = obtenerHumMax();

  if (manualHum) {
    if (humedad < humMin) {
      digitalWrite(PIN_RELAY_HUM, LOW);
      estadoHumificador = true;
    } else if (humedad > humMax) {
      digitalWrite(PIN_RELAY_HUM, HIGH);
      estadoHumificador = false;
    }
    return;
  }

  if (humedad < humMin && !estadoHumificador) {
    digitalWrite(PIN_RELAY_HUM, LOW);
    estadoHumificador = true;
  } else if (humedad > humMax && estadoHumificador) {
    digitalWrite(PIN_RELAY_HUM, HIGH);
    estadoHumificador = false;
  }
}

void iniciarVolteo() {
  if (motorVolteando || enLockdown) return;
  motorVolteando = true;
  inicioVolteo = millis();
  digitalWrite(PIN_RELAY_MOTOR, LOW);
}

void detenerVolteo() {
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  motorVolteando = false;
  ultimoVolteo = obtenerUptimeTotal();
}

void verificarVolteo(unsigned long ahora) {
  if (enLockdown) return;

  if (motorVolteando) {
    if (ahora - inicioVolteo >= DURACION_VOLTEO) {
      detenerVolteo();
    }
    return;
  }

  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }

  if (obtenerUptimeTotal() - tv >= INTERVALO_VOLTEO) {
    iniciarVolteo();
  }
}

void leerSensor() {
  auto status = am2302.read();
  if (status == AM2302::AM2302_READ_OK) {
    temperatura = am2302.get_Temperature();
    humedad = am2302.get_Humidity();
  }
}

unsigned long calcularTiempoVolteo() {
  if (enLockdown) return 0;
  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }
  unsigned long elapsed = obtenerUptimeTotal() - tv;
  if (elapsed >= INTERVALO_VOLTEO) return 0;
  return (INTERVALO_VOLTEO - elapsed) / 1000UL;
}

// ===================== PANTALLA TFT =====================

void dibujarPantalla() {
  gfx->fillScreen(COLOR_BLACK);

  // ---- TITULO ----
  gfx->setCursor(15, 0);
  gfx->setTextColor(COLOR_YELLOW);
  gfx->setTextSize(3);
  gfx->println("INCUBADORA");

  // ---- TEMPERATURA ----
  gfx->setCursor(0, 32);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->print("Temperatura");

  if (temperatura >= TEMP_MIN && temperatura <= TEMP_MAX) {
    gfx->setTextColor(COLOR_GREEN);
  } else {
    gfx->setTextColor(COLOR_RED);
  }
  gfx->setTextSize(5);
  gfx->setCursor(0, 52);
  gfx->print(temperatura, 1);

  // Estado calefactor al lado del valor
  gfx->setTextSize(2);
  gfx->setCursor(120, 60);
  if (estadoCalefactor) {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("CAL ON");
  } else {
    gfx->setTextColor(COLOR_RED);
    gfx->println("CAL OFF");
  }

  // ---- HUMEDAD ----
  gfx->setCursor(0, 100);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->print("Humedad");

  float humMin = obtenerHumMin();
  float humMax = obtenerHumMax();
  if (humedad >= humMin && humedad <= humMax) {
    gfx->setTextColor(COLOR_GREEN);
  } else {
    gfx->setTextColor(COLOR_RED);
  }
  gfx->setTextSize(5);
  gfx->setCursor(0, 120);
  gfx->print(humedad, 0);
  gfx->print("%");

  // Estado humificador al lado
  gfx->setTextSize(2);
  gfx->setCursor(120, 128);
  if (estadoHumificador) {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("HUM ON");
  } else {
    gfx->setTextColor(COLOR_RED);
    gfx->println("HUM OFF");
  }

  // ---- SEPARADOR ----
  gfx->drawFastHLine(0, 175, 170, COLOR_DKGREY);

  // ---- VOLTEO / LOCKDOWN ----
  if (enLockdown) {
    gfx->setCursor(0, 185);
    gfx->setTextColor(COLOR_MAGENTA);
    gfx->setTextSize(3);
    gfx->println("LOCKDOWN");
  } else {
    gfx->setCursor(0, 185);
    gfx->setTextColor(COLOR_WHITE);
    gfx->setTextSize(2);
    gfx->print("Volteo: ");

    if (motorVolteando) {
      gfx->setTextColor(COLOR_ORANGE);
      gfx->println("GIRANDO");
    } else {
      unsigned long restante = calcularTiempoVolteo();
      int horas = restante / 3600;
      int minutos = (restante % 3600) / 60;
      gfx->setTextColor(COLOR_CYAN);
      gfx->print(horas);
      gfx->print("h ");
      gfx->print(minutos);
      gfx->println("m");
    }
  }

  // ---- DIA + FASE ----
  gfx->setCursor(0, 220);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(3);
  gfx->print("D");
  gfx->print(diaActual);
  gfx->print("/");
  gfx->println(DIAS_INCUBACION);

  gfx->setTextSize(2);
  gfx->setCursor(0, 248);
  if (enLockdown) {
    gfx->setTextColor(COLOR_MAGENTA);
    gfx->println("ECLOSION");
  } else {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("DESARROLLO");
  }

  // ---- BARRA DE PROGRESO ----
  int barraAncho = 160;
  int barraProgreso = (int)((float)diaActual / DIAS_INCUBACION * barraAncho);
  gfx->drawRect(5, 275, barraAncho, 14, COLOR_WHITE);
  if (enLockdown) {
    gfx->fillRect(6, 276, barraProgreso - 1, 12, COLOR_MAGENTA);
  } else {
    gfx->fillRect(6, 276, barraProgreso - 1, 12, COLOR_GREEN);
  }

  // ---- PROTECCION ----
  gfx->setCursor(0, 295);
  gfx->setTextColor(COLOR_BLUE);
  gfx->setTextSize(1);
  gfx->println("Protegido contra cortes");

  // ---- COMANDOS ----
  gfx->setCursor(0, 308);
  gfx->setTextColor(COLOR_DKGREY);
  gfx->setTextSize(1);
  if (enLockdown) {
    gfx->println("+/-:temp s:save d:info");
  } else {
    gfx->println("+/-:temp h:hum t:vol s:save");
  }
}

// ===================== SERIAL COMANDOS =====================

void procesarSerial() {
  if (!Serial.available()) return;

  char cmd = Serial.read();

  switch (cmd) {
    case '+':
      tempObjetivo += 0.5;
      Serial.print("Temp objetivo: ");
      Serial.print(tempObjetivo, 1);
      Serial.println(" C");
      break;
    case '-':
      tempObjetivo -= 0.5;
      Serial.print("Temp objetivo: ");
      Serial.print(tempObjetivo, 1);
      Serial.println(" C");
      break;
    case 'h':
      manualHum = !manualHum;
      Serial.print("Humificador manual: ");
      Serial.println(manualHum ? "ON" : "OFF");
      if (!manualHum) {
        digitalWrite(PIN_RELAY_HUM, HIGH);
        estadoHumificador = false;
      }
      break;
    case 't':
      if (enLockdown) {
        Serial.println(F("Volteo bloqueado en LOCKDOWN."));
      } else {
        Serial.println(F("Volteo forzado..."));
        iniciarVolteo();
      }
      break;
    case 'r':
      borrarEstado();
      break;
    case 's':
      guardarEstado();
      break;
    case 'c':
      manualCal = !manualCal;
      Serial.print("Calefactor manual: ");
      Serial.println(manualCal ? "ON" : "OFF");
      if (!manualCal) {
        digitalWrite(PIN_RELAY_HEAT, HIGH);
        estadoCalefactor = false;
      }
      break;
    case 'd':
      Serial.println(F("--- INFO INCUBADORA ---"));
      Serial.print("Dia: ");
      Serial.print(diaActual);
      Serial.print("/");
      Serial.println(DIAS_INCUBACION);
      Serial.print("Fase: ");
      Serial.println(enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO");
      Serial.print("Temp: ");
      Serial.print(temperatura, 1);
      Serial.print("C / Obj: ");
      Serial.print(tempObjetivo, 1);
      Serial.println("C");
      Serial.print("Humedad: ");
      Serial.print(humedad, 1);
      Serial.print("% / Obj: ");
      Serial.print(obtenerHumMin(), 0);
      Serial.print("-");
      Serial.print(obtenerHumMax(), 0);
      Serial.println("%");
      Serial.print("Cal: ");
      Serial.print(estadoCalefactor ? "ON" : "OFF");
      Serial.print("  Hum: ");
      Serial.print(estadoHumificador ? "ON" : "OFF");
      Serial.print("  Motor: ");
      Serial.println(motorVolteando ? "ON" : "OFF");
      Serial.print("Uptime: ");
      Serial.print(obtenerUptimeTotal() / 3600000UL);
      Serial.println(" horas");
      Serial.print("Proteccion flash: ACTIVA");
      Serial.println();
      break;
  }
}

// ===================== SETUP =====================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println(F("\n=== Incubadora Automatica v2.0 ==="));

  // Pines relay
  pinMode(PIN_RELAY_HEAT, OUTPUT);
  pinMode(PIN_RELAY_HUM, OUTPUT);
  pinMode(PIN_RELAY_MOTOR, OUTPUT);

  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);

  // Pantalla
  if (!gfx->begin()) {
    Serial.println(F("ERROR: Pantalla TFT no detectada!"));
    while (true) { delay(10000); }
  }
  Serial.println(F("Pantalla TFT OK"));

  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextColor(COLOR_GREEN);
  gfx->setCursor(10, 30);
  gfx->setTextSize(4);
  gfx->println("AVICORD");
  gfx->setCursor(15, 80);
  gfx->setTextSize(2);
  gfx->println("Incubadora v2.0");
  gfx->setCursor(15, 110);
  gfx->setTextSize(2);
  gfx->println("Iniciando...");
  delay(2000);

  // Sensor
  am2302.begin();
  delay(3000);
  for (int i = 0; i < 5; i++) {
    am2302.read();
    delay(2000);
  }

  // Cargar estado persistente
  cargarEstado();

  // Calcular dia con uptime acumulado
  calcularDia();
  verificarFase();

  // Inicializar timestamps
  ultimoVolteo = obtenerUptimeTotal();
  ultimaLectura = millis();
  ultimaPantalla = millis();
  ultimaGuardado = millis();
  millisEnUltimoGuardado = millis();

  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(DIAS_INCUBACION);
  Serial.print(F("Fase: "));
  Serial.println(enLockdown ? F("LOCKDOWN") : F("DESARROLLO"));
  Serial.println(F("Incubadora lista."));
  Serial.println(F("Cmd: +/-:temp h:hum t:vol s:save d:info r:reset c:calefactor"));
}

// ===================== LOOP =====================

void loop() {
  unsigned long ahora = millis();

  // Actualizar uptime acumulado
  uptimeAcumulado += (ahora - millisEnUltimoGuardado);
  millisEnUltimoGuardado = ahora;

  // Leer sensor y calcular fase
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    leerSensor();
    calcularDia();
    verificarFase();
    ultimaLectura = ahora;

    Serial.print("T:");
    Serial.print(temperatura, 1);
    Serial.print("C  H:");
    Serial.print(humedad, 1);
    Serial.print("%  Cal:");
    Serial.print(estadoCalefactor ? "ON" : "OFF");
    Serial.print("  Hum:");
    Serial.print(estadoHumificador ? "ON" : "OFF");
    Serial.print("  Motor:");
    Serial.print(motorVolteando ? "ON" : "OFF");
    Serial.print("  D:");
    Serial.print(diaActual);
    Serial.println(enLockdown ? " LCK" : " DES");
  }

  // Controlar actuadores
  controlarCalefactor();
  controlarHumificador();
  verificarVolteo(ahora);

  // Guardar estado periodicamente
  if (ahora - ultimaGuardado >= INTERVALO_GUARDADO) {
    guardarEstado();
    ultimaGuardado = ahora;
  }

  // Actualizar pantalla
  if (ahora - ultimaPantalla >= INTERVALO_PANTALLA) {
    dibujarPantalla();
    ultimaPantalla = ahora;
  }

  // Leer comandos seriales
  procesarSerial();
}
