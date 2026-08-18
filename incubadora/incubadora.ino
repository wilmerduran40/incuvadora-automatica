/*
 * Incubadora Automatica - Version Minima Estable
 * ESP32 + DHT22 + TFT ST7789 + Relay module 3 canales
 *
 * Telegram: solo lectura de estado y alertas por temperatura fuera de rango.
 * Eliminado: dashboard web, modo AP, OTA, tarea FreeRTOS, perfiles predefinidos.
 */

#include <AM2302-Sensor.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_task_wdt.h>

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

WiFiClientSecure secured_client;
UniversalTelegramBot bot("", secured_client);

// ===================== RED / BOT =====================
String wifiSsid = "";
String wifiPass = "";
String telegramToken = "";
String chatsPermitidos = "";
unsigned long ultimoIntentoWifi = 0;
bool tiempoSincronizado = false;
bool botListo = false;
bool comandosRegistrados = false;
bool alarmaTemp = false;

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
const unsigned long INTERVALO_LECTURA    = 2000UL;
const unsigned long INTERVALO_PANTALLA   = 1000UL;
const unsigned long INTERVALO_GUARDADO   = 60000UL;
const unsigned long INTERVALO_BOT        = 30000UL;
const unsigned long INTERVALO_RECONEXION_WIFI = 30000UL;
const unsigned long DELAY_ACTUADORES_MS  = 2000UL;
const unsigned long STAGGER_RELAY_MS     = 150UL;
const unsigned long MOTOR_TIMEOUT_MS     = 30000UL;
const int MAX_ERRORES_SENSOR             = 5;

// ===================== PARAMETROS PERSONALIZADOS =====================
float tempObjetivo      = 37.6;
float tempMin           = 37.5;
float tempMax           = 37.8;
float humMinDesarrollo  = 50.0;
float humMaxDesarrollo  = 55.0;
float humMinLockdown    = 65.0;
float humMaxLockdown    = 70.0;
int diasTotal           = 21;
int diaLockdown         = 18;
unsigned long intervaloVolteoMs = 2UL * 60UL * 60UL * 1000UL;
unsigned long duracionVolteoMs  = 20000UL;

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
unsigned long ultimoBot = 0;
unsigned long uptimeAcumulado = 0;
unsigned long millisEnUltimoGuardado = 0;
unsigned long millisArranque = 0;
unsigned long ultimoCambioRelay = 0;

int diaActual = 0;
bool manualCal = false;
bool manualHum = false;
bool sensorValido = false;
int erroresSensor = 0;

bool displayOk = false;
bool actuadoresListos = false;

// ===================== PERSISTENCIA =====================
void guardarEstado() {
  preferences.begin("incubadora", false);
  unsigned long uptimeTotal = uptimeAcumulado + millis();
  preferences.putULong("uptime", uptimeTotal);
  preferences.putULong("ultimoVol", ultimoVolteo);
  preferences.putBool("manCal", manualCal);
  preferences.putBool("manHum", manualHum);
  preferences.putFloat("tempObj", tempObjetivo);
  preferences.putFloat("tempMin", tempMin);
  preferences.putFloat("tempMax", tempMax);
  preferences.putFloat("humMinDev", humMinDesarrollo);
  preferences.putFloat("humMaxDev", humMaxDesarrollo);
  preferences.putFloat("humMinLock", humMinLockdown);
  preferences.putFloat("humMaxLock", humMaxLockdown);
  preferences.putInt("diasTotal", diasTotal);
  preferences.putInt("diaLockdown", diaLockdown);
  preferences.putULong("intVolteo", intervaloVolteoMs);
  preferences.putULong("durVolteo", duracionVolteoMs);
  preferences.end();
}

void cargarEstado() {
  preferences.begin("incubadora", true);
  unsigned long uptimeGuardado = preferences.getULong("uptime", 0);
  ultimoVolteo = preferences.getULong("ultimoVol", 0);
  manualCal = preferences.getBool("manCal", false);
  manualHum = preferences.getBool("manHum", false);
  tempObjetivo     = preferences.getFloat("tempObj", tempObjetivo);
  tempMin          = preferences.getFloat("tempMin", tempMin);
  tempMax          = preferences.getFloat("tempMax", tempMax);
  humMinDesarrollo = preferences.getFloat("humMinDev", humMinDesarrollo);
  humMaxDesarrollo = preferences.getFloat("humMaxDev", humMaxDesarrollo);
  humMinLockdown   = preferences.getFloat("humMinLock", humMinLockdown);
  humMaxLockdown   = preferences.getFloat("humMaxLock", humMaxLockdown);
  diasTotal        = preferences.getInt("diasTotal", diasTotal);
  diaLockdown      = preferences.getInt("diaLockdown", diaLockdown);
  intervaloVolteoMs = preferences.getULong("intVolteo", intervaloVolteoMs);
  duracionVolteoMs  = preferences.getULong("durVolteo", duracionVolteoMs);
  preferences.end();
  uptimeAcumulado = uptimeGuardado;
  millisEnUltimoGuardado = millis();
}

void borrarEstado() {
  preferences.begin("incubadora", false);
  preferences.clear();
  preferences.end();
  uptimeAcumulado = 0;
  ultimoVolteo = 0;
  millisEnUltimoGuardado = millis();
  manualCal = false;
  manualHum = false;
  tempObjetivo      = 37.6;
  tempMin           = 37.5;
  tempMax           = 37.8;
  humMinDesarrollo  = 50.0;
  humMaxDesarrollo  = 55.0;
  humMinLockdown    = 65.0;
  humMaxLockdown    = 70.0;
  diasTotal         = 21;
  diaLockdown       = 18;
  intervaloVolteoMs = 2UL * 60UL * 60UL * 1000UL;
  duracionVolteoMs  = 20000UL;
  Serial.println(F("Estado borrado. Reiniciando."));
  delay(500);
  ESP.restart();
}

void guardarRed() {
  preferences.begin("incubadora", false);
  preferences.putString("wifi_ssid", wifiSsid);
  preferences.putString("wifi_pass", wifiPass);
  preferences.putString("token_tg", telegramToken);
  preferences.putString("chats", chatsPermitidos);
  preferences.end();
}

void cargarRed() {
  preferences.begin("incubadora", true);
  wifiSsid = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");
  telegramToken = preferences.getString("token_tg", "");
  chatsPermitidos = preferences.getString("chats", "");
  preferences.end();
}

// ===================== WIFI =====================
void configurarWifi(const String& ssid, const String& pass) {
  wifiSsid = ssid;
  wifiPass = pass;
  guardarRed();
  Serial.print(F("WiFi guardado. Conectando a: "));
  Serial.println(ssid);
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  ultimoIntentoWifi = millis();
}

void gestionarWifi() {
  static bool ntpArrancado = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpArrancado) {
      configTime(0, 0, "pool.ntp.org");
      ntpArrancado = true;
    }
    ultimoIntentoWifi = millis();
    return;
  }
  if (wifiSsid.length() == 0) return;
  if (millis() - ultimoIntentoWifi < INTERVALO_RECONEXION_WIFI) return;
  ultimoIntentoWifi = millis();
  Serial.print(F("Reconectando WiFi: "));
  Serial.println(wifiSsid);
  if (WiFi.status() == WL_DISCONNECTED || WiFi.status() == WL_NO_SSID_AVAIL) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}

void mostrarEstadoWifi() {
  Serial.println(F("--- RED ---"));
  Serial.print(F("SSID: "));
  Serial.println(wifiSsid.length() > 0 ? wifiSsid : "(no configurado)");
  Serial.print(F("Estado: "));
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP: "));
    Serial.println(WiFi.localIP());
  }
  Serial.print(F("Token bot: "));
  Serial.println(telegramToken.length() > 0 ? "SI" : "NO");
  Serial.print(F("Chats permitidos: "));
  Serial.println(chatsPermitidos.length() > 0 ? chatsPermitidos : "(ninguno)");
}

// ===================== CHATS PERMITIDOS =====================
bool chatPermitido(const String& id) {
  String resto = chatsPermitidos;
  while (resto.length() > 0) {
    int coma = resto.indexOf(',');
    String cur = (coma >= 0) ? resto.substring(0, coma) : resto;
    cur.trim();
    if (cur == id) return true;
    if (coma >= 0) resto = resto.substring(coma + 1);
    else break;
  }
  return false;
}

void agregarChat(const String& id) {
  if (chatPermitido(id)) return;
  if (chatsPermitidos.length() > 0) chatsPermitidos += ",";
  chatsPermitidos += id;
  guardarRed();
  Serial.print(F("Chat permitido: "));
  Serial.println(id);
}

void quitarChat(const String& id) {
  String nuevo;
  String resto = chatsPermitidos;
  while (resto.length() > 0) {
    int coma = resto.indexOf(',');
    String cur = (coma >= 0) ? resto.substring(0, coma) : resto;
    cur.trim();
    if (cur != id && cur.length() > 0) {
      if (nuevo.length() > 0) nuevo += ",";
      nuevo += cur;
    }
    if (coma >= 0) resto = resto.substring(coma + 1);
    else break;
  }
  chatsPermitidos = nuevo;
  guardarRed();
  Serial.print(F("Chat bloqueado: "));
  Serial.println(id);
}

void configurarToken(const String& tok) {
  telegramToken = tok;
  telegramToken.trim();
  guardarRed();
  bot.updateToken(telegramToken);
  comandosRegistrados = false;
  Serial.println(F("Token Telegram actualizado."));
}

// ===================== LOGICA DE FASES =====================
unsigned long obtenerUptimeTotal() {
  return uptimeAcumulado + millis();
}

void calcularDia() {
  unsigned long uptimeTotal = obtenerUptimeTotal();
  diaActual = (int)(uptimeTotal / 86400000UL) + 1;
  if (diaActual > diasTotal) diaActual = diasTotal;
}

void verificarFase() {
  bool eraLockdown = enLockdown;
  enLockdown = (diaActual >= diaLockdown);
  if (enLockdown && !eraLockdown) {
    Serial.println(F("*** LOCKDOWN ***"));
    if (motorVolteando) detenerVolteo();
  }
}

float obtenerHumMin() {
  return enLockdown ? humMinLockdown : humMinDesarrollo;
}

float obtenerHumMax() {
  return enLockdown ? humMaxLockdown : humMaxDesarrollo;
}

// ===================== CONTROL DE ACTUADORES =====================
void relaysInitSeguro() {
  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  pinMode(PIN_RELAY_HEAT, OUTPUT);
  pinMode(PIN_RELAY_HUM, OUTPUT);
  pinMode(PIN_RELAY_MOTOR, OUTPUT);
  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  estadoCalefactor = false;
  estadoHumificador = false;
  motorVolteando = false;
  actuadoresListos = false;
  ultimoCambioRelay = 0;
}

bool relayPuedeCambiar() {
  unsigned long ahora = millis();
  if (ultimoCambioRelay != 0 && (ahora - ultimoCambioRelay) < STAGGER_RELAY_MS) {
    return false;
  }
  ultimoCambioRelay = ahora;
  return true;
}

void apagarActuadoresSeguro() {
  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  estadoCalefactor = false;
  estadoHumificador = false;
}

void controlarCalefactor() {
  if (!actuadoresListos || !sensorValido || manualCal) return;
  if (temperatura < tempMin && !estadoCalefactor) {
    if (!relayPuedeCambiar()) return;
    digitalWrite(PIN_RELAY_HEAT, LOW);
    estadoCalefactor = true;
  } else if (temperatura > tempMax && estadoCalefactor) {
    if (!relayPuedeCambiar()) return;
    digitalWrite(PIN_RELAY_HEAT, HIGH);
    estadoCalefactor = false;
  }
}

void controlarHumificador() {
  if (!actuadoresListos || !sensorValido) return;
  float humMin = obtenerHumMin();
  float humMax = obtenerHumMax();
  if (humedad < humMin && !estadoHumificador) {
    if (!relayPuedeCambiar()) return;
    digitalWrite(PIN_RELAY_HUM, LOW);
    estadoHumificador = true;
  } else if (humedad > humMax && estadoHumificador) {
    if (!relayPuedeCambiar()) return;
    digitalWrite(PIN_RELAY_HUM, HIGH);
    estadoHumificador = false;
  }
}

void iniciarVolteo() {
  if (!actuadoresListos || !sensorValido || motorVolteando || enLockdown) return;
  if (!relayPuedeCambiar()) return;
  motorVolteando = true;
  inicioVolteo = millis();
  digitalWrite(PIN_RELAY_MOTOR, LOW);
}

void detenerVolteo() {
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  motorVolteando = false;
  ultimoVolteo = obtenerUptimeTotal();
  ultimoCambioRelay = millis();
}

void verificarVolteo(unsigned long ahora) {
  if (enLockdown) return;
  if (motorVolteando) {
    if (ahora - inicioVolteo >= duracionVolteoMs) {
      detenerVolteo();
    }
    return;
  }
  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }
  if (obtenerUptimeTotal() - tv >= intervaloVolteoMs) {
    iniciarVolteo();
  }
}

void leerSensor() {
  unsigned long t0 = millis();
  auto status = am2302.read();
  unsigned long duracion = millis() - t0;
  if (status == AM2302::AM2302_READ_OK) {
    float t = am2302.get_Temperature();
    float h = am2302.get_Humidity();
    if (t >= 10.0 && t <= 50.0 && h >= 5.0 && h <= 100.0) {
      temperatura = t;
      humedad = h;
      erroresSensor = 0;
      if (!sensorValido) {
        sensorValido = true;
        Serial.println(F("Sensor DHT22 valido."));
      }
    } else {
      erroresSensor++;
      Serial.println(F("Lectura DHT fuera de rango."));
    }
    if (duracion > 500) {
      Serial.print(F("Lectura DHT lenta: "));
      Serial.print(duracion);
      Serial.println(F(" ms"));
    }
  } else {
    erroresSensor++;
    Serial.print(F("ERROR DHT ("));
    Serial.print(status);
    Serial.print(F("), duracion "));
    Serial.print(duracion);
    Serial.println(F(" ms"));
  }
  if (erroresSensor >= MAX_ERRORES_SENSOR && sensorValido) {
    sensorValido = false;
    Serial.println(F("ALERTA: sensor fallando. Apagando calefactor/humificador."));
    apagarActuadoresSeguro();
  }
}

unsigned long calcularTiempoVolteo() {
  if (enLockdown) return 0;
  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }
  unsigned long elapsed = obtenerUptimeTotal() - tv;
  if (elapsed >= intervaloVolteoMs) return 0;
  return (intervaloVolteoMs - elapsed) / 1000UL;
}

// ===================== COMANDOS COMPARTIDOS =====================
void subirTemp() {
  tempObjetivo += 0.5;
  Serial.print(F("Temp objetivo: "));
  Serial.print(tempObjetivo, 1);
  Serial.println(F(" C"));
}

void bajarTemp() {
  tempObjetivo -= 0.5;
  Serial.print(F("Temp objetivo: "));
  Serial.print(tempObjetivo, 1);
  Serial.println(F(" C"));
}

void toggleHumManual() {
  manualHum = !manualHum;
  Serial.print(F("Humificador manual: "));
  Serial.println(manualHum ? "ON" : "OFF");
  if (!manualHum) {
    digitalWrite(PIN_RELAY_HUM, HIGH);
    estadoHumificador = false;
  }
}

void toggleCalManual() {
  manualCal = !manualCal;
  Serial.print(F("Calefactor manual: "));
  Serial.println(manualCal ? "ON" : "OFF");
  if (!manualCal) {
    digitalWrite(PIN_RELAY_HEAT, HIGH);
    estadoCalefactor = false;
  }
}

void forzarVolteoManual() {
  if (enLockdown) {
    Serial.println(F("Volteo bloqueado en LOCKDOWN."));
    return;
  }
  Serial.println(F("Volteo forzado..."));
  iniciarVolteo();
}

void mostrarInfoSerial() {
  Serial.println(F("--- INFO INCUBADORA ---"));
  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(diasTotal);
  Serial.print(F("Fase: "));
  Serial.println(enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO");
  Serial.print(F("Temp: "));
  Serial.print(temperatura, 1);
  Serial.print(F("C / Obj: "));
  Serial.print(tempObjetivo, 1);
  Serial.println(F("C"));
  Serial.print(F("Humedad: "));
  Serial.print(humedad, 1);
  Serial.print(F("% / Obj: "));
  Serial.print(obtenerHumMin(), 0);
  Serial.print(F("-"));
  Serial.print(obtenerHumMax(), 0);
  Serial.println(F("%"));
  Serial.print(F("Cal: "));
  Serial.print(estadoCalefactor ? "ON" : "OFF");
  Serial.print(F("  Hum: "));
  Serial.print(estadoHumificador ? "ON" : "OFF");
  Serial.print(F("  Motor: "));
  Serial.println(motorVolteando ? "ON" : "OFF");
  Serial.print(F("Uptime: "));
  Serial.print(obtenerUptimeTotal() / 3600000UL);
  Serial.println(F(" horas"));
  Serial.print(F("WiFi: "));
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");
  Serial.print(F("Bot: "));
  Serial.println(botListo ? "ACTIVO" : "INACTIVO");
  Serial.print(F("Heap libre: "));
  Serial.println(ESP.getFreeHeap());
}

String obtenerInfoTelegram() {
  char buf[512];
  unsigned long uptimeH = obtenerUptimeTotal() / 3600000UL;
  unsigned long rest = calcularTiempoVolteo();
  snprintf(buf, sizeof(buf),
    "INCUBADORA\n"
    "Dia: %d/%d\n"
    "Fase: %s\n"
    "Temp: %.1fC (obj %.1fC)\n"
    "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
    "Calefactor: %s\n"
    "Humificador: %s\n"
    "Motor: %s\n"
    "Prox. volteo: %luh %lum\n"
    "Uptime: %luh",
    diaActual, diasTotal,
    enLockdown ? "LOCKDOWN" : "DESARROLLO",
    temperatura, tempObjetivo,
    humedad, obtenerHumMin(), obtenerHumMax(),
    estadoCalefactor ? "ON" : "OFF",
    estadoHumificador ? "ON" : "OFF",
    motorVolteando ? "GIRANDO" : "OFF",
    rest / 3600, (rest % 3600) / 60,
    uptimeH);
  return String(buf);
}

String obtenerAyudaTelegram() {
  String s;
  s += "COMANDOS:\n";
  s += "info - estado completo\n";
  s += "help - esta ayuda\n";
  s += "id - tu chat_id";
  return s;
}

void mostrarAyudaSerial() {
  Serial.println(F("--- COMANDOS ---"));
  Serial.println(F("+/-           subir/bajar temp objetivo"));
  Serial.println(F("temp 38.0     fijar temp objetivo"));
  Serial.println(F("h             toggle humificador manual"));
  Serial.println(F("c             toggle calefactor manual"));
  Serial.println(F("t             volteo forzado"));
  Serial.println(F("s             guardar estado"));
  Serial.println(F("r             reset estado"));
  Serial.println(F("d / info      mostrar info"));
  Serial.println(F("set dias X    dias totales incubacion"));
  Serial.println(F("set lock X    dia inicio lockdown"));
  Serial.println(F("set tobj X    temp objetivo"));
  Serial.println(F("set tmin X    temp minima"));
  Serial.println(F("set tmax X    temp maxima"));
  Serial.println(F("set hmin X    hum min desarrollo"));
  Serial.println(F("set hmax X    hum max desarrollo"));
  Serial.println(F("set hlmin X   hum min lockdown"));
  Serial.println(F("set hlmax X   hum max lockdown"));
  Serial.println(F("set vol X     intervalo volteo (horas)"));
  Serial.println(F("set dur X     duracion volteo (segundos)"));
  Serial.println(F("params        mostrar parametros"));
  Serial.println(F("wifi ssid pass"));
  Serial.println(F("token TOKEN"));
  Serial.println(F("allow/block ID"));
  Serial.println(F("delwifi / deltoken"));
  Serial.println(F("help          esta ayuda"));
}

// ===================== BOT TELEGRAM =====================
void notificarTodos(const String& msg) {
  if (!botListo) return;
  String resto = chatsPermitidos;
  while (resto.length() > 0) {
    int coma = resto.indexOf(',');
    String id = (coma >= 0) ? resto.substring(0, coma) : resto;
    id.trim();
    if (id.length() > 0) {
      bot.sendMessage(id, msg, "");
      delay(10);
    }
    if (coma >= 0) resto = resto.substring(coma + 1);
    else break;
  }
}

void manejarMensajes(int num) {
  for (int i = 0; i < num; i++) {
    String chat = bot.messages[i].chat_id;
    if (chat.length() == 0) continue;
    String texto = bot.messages[i].text;
    texto.trim();
    String textoLow = texto;
    textoLow.toLowerCase();
    if (textoLow.startsWith("/")) textoLow = textoLow.substring(1);
    int sp = textoLow.indexOf(' ');
    String cmd = textoLow;
    if (sp >= 0) cmd = textoLow.substring(0, sp);
    if (cmd.length() == 0) continue;

    if (cmd == "start") {
      if (chatsPermitidos.length() == 0) agregarChat(chat);
      if (chatPermitido(chat)) {
        bot.sendMessage(chat, "Hola! Bot de incubadora.\n\n" + obtenerAyudaTelegram(), "");
      } else {
        bot.sendMessage(chat, "No autorizado. Pide acceso: allow " + chat, "");
      }
      continue;
    }
    if (cmd == "id") {
      bot.sendMessage(chat, "Tu chat_id: " + chat, "");
      continue;
    }
    if (cmd == "help" || cmd == "ayuda") {
      bot.sendMessage(chat, obtenerAyudaTelegram(), "");
      continue;
    }
    if (!chatPermitido(chat)) {
      bot.sendMessage(chat, "No autorizado.", "");
      continue;
    }
    if (cmd == "info" || cmd == "estado" || cmd == "status") {
      bot.sendMessage(chat, obtenerInfoTelegram(), "");
    } else {
      bot.sendMessage(chat, "Comando desconocido. Usa: help", "");
    }
  }
}

void procesarTelegram() {
  if (!botListo) return;
  if (millis() - ultimoBot < INTERVALO_BOT) return;
  ultimoBot = millis();
  if (!comandosRegistrados) {
    String cmds = "[{\"command\":\"start\",\"description\":\"Bienvenida\"},{\"command\":\"help\",\"description\":\"Ayuda\"},{\"command\":\"info\",\"description\":\"Estado completo\"}]";
    bot.setMyCommands(cmds);
    comandosRegistrados = true;
  }
  int num = bot.getUpdates(bot.last_message_received + 1);
  if (num > 0) {
    manejarMensajes(num);
  }
}

void verificarAlertas() {
  if (!botListo || temperatura <= 0.0 || !sensorValido) return;
  bool fuera = (temperatura < tempMin || temperatura > tempMax);
  if (fuera && !alarmaTemp) {
    alarmaTemp = true;
    notificarTodos("ALERTA: temperatura fuera de rango: " + String(temperatura, 1) + "C");
  } else if (!fuera && alarmaTemp) {
    alarmaTemp = false;
    notificarTodos("OK: temperatura en rango: " + String(temperatura, 1) + "C");
  }
}

// ===================== PANTALLA TFT =====================
void dibujarPantalla() {
  if (!displayOk) return;
  gfx->fillScreen(COLOR_BLACK);

  gfx->setCursor(15, 0);
  gfx->setTextColor(COLOR_YELLOW);
  gfx->setTextSize(3);
  gfx->println("INCUBADORA");

  gfx->setTextSize(1);
  gfx->setCursor(0, 24);
  gfx->setTextColor(COLOR_CYAN);
  gfx->print("Personalizado");

  gfx->setCursor(0, 32);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->print("Temperatura");

  if (temperatura >= tempMin && temperatura <= tempMax) {
    gfx->setTextColor(COLOR_GREEN);
  } else {
    gfx->setTextColor(COLOR_RED);
  }
  gfx->setTextSize(5);
  gfx->setCursor(0, 52);
  gfx->print(temperatura, 1);

  gfx->setTextSize(2);
  gfx->setCursor(120, 60);
  if (estadoCalefactor) {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("CAL ON");
  } else {
    gfx->setTextColor(COLOR_RED);
    gfx->println("CAL OFF");
  }

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

  gfx->setTextSize(2);
  gfx->setCursor(120, 128);
  if (estadoHumificador) {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("HUM ON");
  } else {
    gfx->setTextColor(COLOR_RED);
    gfx->println("HUM OFF");
  }

  gfx->drawFastHLine(0, 175, 170, COLOR_DKGREY);

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

  gfx->setCursor(0, 220);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(3);
  gfx->print("D");
  gfx->print(diaActual);
  gfx->print("/");
  gfx->println(diasTotal);

  gfx->setTextSize(2);
  gfx->setCursor(0, 248);
  if (enLockdown) {
    gfx->setTextColor(COLOR_MAGENTA);
    gfx->println("ECLOSION");
  } else {
    gfx->setTextColor(COLOR_GREEN);
    gfx->println("DESARROLLO");
  }

  int barraAncho = 160;
  int barraProgreso = (int)((float)diaActual / diasTotal * barraAncho);
  gfx->drawRect(5, 275, barraAncho, 14, COLOR_WHITE);
  if (barraProgreso > 1) {
    if (enLockdown) {
      gfx->fillRect(6, 276, barraProgreso - 1, 12, COLOR_MAGENTA);
    } else {
      gfx->fillRect(6, 276, barraProgreso - 1, 12, COLOR_GREEN);
    }
  }

  gfx->setCursor(0, 295);
  gfx->setTextColor(COLOR_BLUE);
  gfx->setTextSize(1);
  gfx->println("Protegido contra cortes");

  gfx->setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->setCursor(140, 296);
    gfx->setTextColor(COLOR_GREEN);
    gfx->print("W");
  } else {
    gfx->setCursor(140, 296);
    gfx->setTextColor(COLOR_RED);
    gfx->print("w");
  }
  if (botListo) {
    gfx->setCursor(150, 296);
    gfx->setTextColor(COLOR_GREEN);
    gfx->print("T");
  } else {
    gfx->setCursor(150, 296);
    gfx->setTextColor(COLOR_RED);
    gfx->print("t");
  }

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
String serialBuf = "";

String quitarPrefijo(String t) {
  t.trim();
  String low = t;
  low.toLowerCase();
  if (low.startsWith("token=")) return t.substring(6);
  if (low.startsWith("ssid=") || low.startsWith("pass=")) return t.substring(5);
  if (low.startsWith("t=")) return t.substring(2);
  if (low.startsWith("s=") || low.startsWith("p=")) return t.substring(2);
  return t;
}

void mostrarParametrosSerial() {
  Serial.println(F("--- PARAMETROS ---"));
  Serial.print(F("Dias totales: ")); Serial.println(diasTotal);
  Serial.print(F("Dia lockdown: ")); Serial.println(diaLockdown);
  Serial.print(F("Temp objetivo: ")); Serial.println(tempObjetivo, 1);
  Serial.print(F("Temp min: ")); Serial.println(tempMin, 1);
  Serial.print(F("Temp max: ")); Serial.println(tempMax, 1);
  Serial.print(F("Hum min dev: ")); Serial.println(humMinDesarrollo, 1);
  Serial.print(F("Hum max dev: ")); Serial.println(humMaxDesarrollo, 1);
  Serial.print(F("Hum min lock: ")); Serial.println(humMinLockdown, 1);
  Serial.print(F("Hum max lock: ")); Serial.println(humMaxLockdown, 1);
  Serial.print(F("Intervalo volteo: ")); Serial.print(intervaloVolteoMs / 3600000.0, 1); Serial.println(F(" h"));
  Serial.print(F("Duracion volteo: ")); Serial.print(duracionVolteoMs / 1000.0, 1); Serial.println(F(" s"));
}

void procesarSet(const String& param, const String& valor) {
  float v = valor.toFloat();
  if (param == "dias") {
    diasTotal = (int)v;
    Serial.print(F("Dias totales: ")); Serial.println(diasTotal);
  } else if (param == "lock") {
    diaLockdown = (int)v;
    Serial.print(F("Dia lockdown: ")); Serial.println(diaLockdown);
  } else if (param == "tobj") {
    tempObjetivo = v;
    Serial.print(F("Temp objetivo: ")); Serial.println(tempObjetivo, 1);
  } else if (param == "tmin") {
    tempMin = v;
    Serial.print(F("Temp min: ")); Serial.println(tempMin, 1);
  } else if (param == "tmax") {
    tempMax = v;
    Serial.print(F("Temp max: ")); Serial.println(tempMax, 1);
  } else if (param == "hmin") {
    humMinDesarrollo = v;
    Serial.print(F("Hum min dev: ")); Serial.println(humMinDesarrollo, 1);
  } else if (param == "hmax") {
    humMaxDesarrollo = v;
    Serial.print(F("Hum max dev: ")); Serial.println(humMaxDesarrollo, 1);
  } else if (param == "hlmin") {
    humMinLockdown = v;
    Serial.print(F("Hum min lock: ")); Serial.println(humMinLockdown, 1);
  } else if (param == "hlmax") {
    humMaxLockdown = v;
    Serial.print(F("Hum max lock: ")); Serial.println(humMaxLockdown, 1);
  } else if (param == "vol") {
    if (v > 0) {
      intervaloVolteoMs = (unsigned long)(v * 3600.0 * 1000.0);
      Serial.print(F("Intervalo volteo: ")); Serial.print(v, 1); Serial.println(F(" h"));
    }
  } else if (param == "dur") {
    if (v > 0) {
      duracionVolteoMs = (unsigned long)(v * 1000.0);
      Serial.print(F("Duracion volteo: ")); Serial.print(v, 1); Serial.println(F(" s"));
    }
  } else {
    Serial.println(F("Parametro desconocido."));
  }
}

void procesarLinea(String linea) {
  linea.trim();
  if (linea.length() == 0) return;
  if (linea.length() == 1) {
    switch (linea[0]) {
      case '+': subirTemp(); return;
      case '-': bajarTemp(); return;
      case 'h': toggleHumManual(); return;
      case 'c': toggleCalManual(); return;
      case 't': forzarVolteoManual(); return;
      case 's': guardarEstado(); return;
      case 'r': borrarEstado(); return;
      case 'd': mostrarInfoSerial(); return;
    }
  }
  int sp = linea.indexOf(' ');
  String cmd = sp >= 0 ? linea.substring(0, sp) : linea;
  String arg = sp >= 0 ? linea.substring(sp + 1) : "";
  arg.trim();
  String cmdLower = cmd;
  cmdLower.toLowerCase();

  if (cmdLower == "wifi") {
    if (arg.length() == 0) {
      mostrarEstadoWifi();
    } else {
      int sp2 = arg.indexOf(' ');
      String ssid = quitarPrefijo(sp2 >= 0 ? arg.substring(0, sp2) : arg);
      String pass = quitarPrefijo(sp2 >= 0 ? arg.substring(sp2 + 1) : "");
      if (ssid.length() == 0) {
        Serial.println(F("Uso: wifi <ssid> <clave>"));
      } else {
        configurarWifi(ssid, pass);
      }
    }
  } else if (cmdLower == "token") {
    if (arg.length() == 0) {
      Serial.println(telegramToken.length() > 0 ? "Token configurado." : "Token NO configurado.");
    } else {
      String tok = quitarPrefijo(arg);
      if (tok.length() < 10 || tok.indexOf(':') < 0) {
        Serial.println(F("Token invalido."));
      } else {
        configurarToken(tok);
      }
    }
  } else if (cmdLower == "delwifi") {
    wifiSsid = "";
    wifiPass = "";
    guardarRed();
    WiFi.disconnect();
    Serial.println(F("Credenciales WiFi borradas."));
  } else if (cmdLower == "deltoken") {
    telegramToken = "";
    bot.updateToken("");
    guardarRed();
    Serial.println(F("Token del bot borrado."));
  } else if (cmdLower == "allow") {
    if (arg.length() == 0) {
      Serial.print(F("Chats: "));
      Serial.println(chatsPermitidos.length() > 0 ? chatsPermitidos : "(ninguno)");
    } else {
      agregarChat(arg);
    }
  } else if (cmdLower == "block") {
    if (arg.length() == 0) {
      Serial.println(F("Uso: block <chat_id>"));
    } else {
      quitarChat(arg);
    }
  } else if (cmdLower == "help") {
    mostrarAyudaSerial();
  } else if (cmdLower == "info") {
    mostrarInfoSerial();
  } else if (cmdLower == "temp") {
    float v = arg.toFloat();
    if (v > 0) {
      tempObjetivo = v;
      Serial.print(F("Temp objetivo: "));
      Serial.print(tempObjetivo, 1);
      Serial.println(F(" C"));
    } else {
      Serial.println(F("Uso: temp 38.0"));
    }
  } else if (cmdLower == "set") {
    int sp2 = arg.indexOf(' ');
    if (sp2 >= 0) {
      String param = arg.substring(0, sp2);
      String valor = arg.substring(sp2 + 1);
      procesarSet(param, valor);
    } else {
      Serial.println(F("Uso: set <param> <valor>"));
    }
  } else if (cmdLower == "params") {
    mostrarParametrosSerial();
  } else {
    Serial.println(F("Comando desconocido. Usa: help"));
  }
}

void procesarSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.length() > 0) {
        procesarLinea(serialBuf);
        serialBuf = "";
      }
    } else {
      serialBuf += c;
    }
  }
  if (serialBuf.length() == 1) {
    char c = serialBuf[0];
    if (c == '+' || c == '-' || c == 'h' || c == 't' || c == 's' || c == 'r' || c == 'c' || c == 'd') {
      procesarLinea(serialBuf);
      serialBuf = "";
    }
  }
}

// ===================== SETUP =====================
void setup() {
  relaysInitSeguro();
  millisArranque = millis();

  Serial.begin(115200);
  Serial.println(F("\n=== Incubadora Automatica - Minima Estable ==="));
  Serial.println(F("Relays OFF (arranque seguro)."));

  // Watchdog a 5 segundos
  esp_task_wdt_config_t twdt_config = {};
  twdt_config.timeout_ms = 5000;
  twdt_config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
  twdt_config.trigger_panic = true;
  esp_task_wdt_reconfigure(&twdt_config);
  Serial.println(F("Watchdog 5s activo."));

  // Pantalla con retry
  for (int intento = 0; intento < 2; intento++) {
    esp_task_wdt_reset();
    if (gfx->begin()) {
      displayOk = true;
      break;
    }
    delay(500);
  }
  if (displayOk) {
    Serial.println(F("Pantalla TFT OK"));
  } else {
    Serial.println(F("ERROR: Pantalla no detectada. Continua sin display."));
  }

  if (displayOk) {
    gfx->fillScreen(COLOR_BLACK);
    gfx->setTextColor(COLOR_GREEN);
    gfx->setCursor(10, 30);
    gfx->setTextSize(4);
    gfx->println("AVICORD");
    gfx->setCursor(15, 80);
    gfx->setTextSize(2);
    gfx->println("Incubadora");
    gfx->setCursor(15, 110);
    gfx->println("Iniciando...");
    delay(200);
  }

  cargarEstado();
  cargarRed();
  calcularDia();
  verificarFase();

  am2302.begin();
  am2302.read();

  ultimoVolteo = obtenerUptimeTotal();
  ultimaLectura = millis();
  ultimaPantalla = millis();
  ultimaGuardado = millis();
  ultimoBot = millis();
  millisEnUltimoGuardado = millis();

  bot.updateToken(telegramToken);
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  secured_client.setTimeout(2);
  bot.waitForResponse = 1000;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (wifiSsid.length() > 0) {
    Serial.print(F("Conectando a WiFi: "));
    Serial.println(wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    ultimoIntentoWifi = millis();
  } else {
    Serial.println(F("Sin WiFi configurado. Funcionamiento local."));
  }

  if (telegramToken.length() == 0) {
    Serial.println(F("Sin token Telegram. Configura: token <BOT_TOKEN>"));
  }

  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(diasTotal);
  Serial.print(F("Fase: "));
  Serial.println(enLockdown ? "LOCKDOWN" : "DESARROLLO");
  Serial.println(F("Incubadora lista."));
  Serial.println(F("Escribe 'help' para ver comandos."));
}

// ===================== LOOP =====================
void loop() {
  unsigned long ahora = millis();
  esp_task_wdt_reset();

  uptimeAcumulado += (ahora - millisEnUltimoGuardado);
  millisEnUltimoGuardado = ahora;

  gestionarWifi();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk && !tiempoSincronizado) {
    time_t now = time(nullptr);
    if (now > 24 * 3600) {
      tiempoSincronizado = true;
      Serial.println(F("Hora NTP sincronizada."));
    }
  }
  botListo = wifiOk && tiempoSincronizado && telegramToken.length() > 0;

  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    leerSensor();
    calcularDia();
    verificarFase();
    ultimaLectura = ahora;
  }

  if (!actuadoresListos && sensorValido && (ahora - millisArranque >= DELAY_ACTUADORES_MS)) {
    actuadoresListos = true;
    Serial.println(F("Actuadores habilitados."));
  }

  controlarCalefactor();
  controlarHumificador();
  verificarVolteo(ahora);

  if (motorVolteando && (ahora - inicioVolteo > MOTOR_TIMEOUT_MS)) {
    Serial.println(F("ALERTA: Motor timeout. Apagando forzado."));
    detenerVolteo();
  }

  if (ahora - ultimaGuardado >= INTERVALO_GUARDADO) {
    guardarEstado();
    ultimaGuardado = ahora;
  }

  if (ahora - ultimaPantalla >= INTERVALO_PANTALLA) {
    dibujarPantalla();
    ultimaPantalla = ahora;
  }

  procesarTelegram();
  verificarAlertas();

  procesarSerial();
}



