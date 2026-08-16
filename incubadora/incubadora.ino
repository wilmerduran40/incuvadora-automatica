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
 *  - UniversalTelegramBot
 *  - ArduinoJson (dependencia del bot)
 *
 * Fases incubacion pollos:
 *  - Dias 1-17:  Desarrollo  (Humedad 50-55%, Volteo ON cada 2h)
 *  - Dias 18-21: Lockdown    (Humedad 65-70%, Volteo OFF)
 *
 * Proteccion contra cortes electricos: Preferences (NVS flash)
 * Comandos serial: +/-:temp temp X h:hum c:cal t:vol s:save d:info r:reset
 * Red:            wifi <ssid> <pass> | token <BOT_TOKEN> | allow/block <chat_id>
 * Bot Telegram:   info, start, help, +, -, temp X, h, c, t, s, reset si
 */

#include <AM2302-Sensor.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <time.h>

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

// ===================== RED / BOT =====================
WiFiClientSecure secured_client;
UniversalTelegramBot bot("", secured_client);

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
const unsigned long INTERVALO_VOLTEO    = 2UL * 60UL * 60UL * 1000UL;
const unsigned long DURACION_VOLTEO     = 20000UL;
const unsigned long INTERVALO_LECTURA   = 2000UL;
const unsigned long INTERVALO_PANTALLA  = 1000UL;
const unsigned long INTERVALO_GUARDADO  = 60000UL;
const unsigned long INTERVALO_BOT       = 3000UL;
const unsigned long INTERVALO_RECONEXION_WIFI = 15000UL;
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
const float TEMP_MIN      = 37.5;
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
unsigned long ultimoBot = 0;
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
  preferences.remove("uptime");
  preferences.remove("ultimoVol");
  preferences.remove("manCal");
  preferences.remove("manHum");
  preferences.end();

  uptimeAcumulado = 0;
  inicioIncubacion = millis();
  ultimoVolteo = millis();
  millisEnUltimoGuardado = millis();
  manualCal = false;
  manualHum = false;

  Serial.println(F("Estado borrado. Reiniciando."));
}

// ===================== PERSISTENCIA DE RED / BOT =====================

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
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

void mostrarEstadoWifi() {
  Serial.println(F("--- RED ---"));
  if (wifiSsid.length() > 0) {
    Serial.print(F("SSID: "));
    Serial.println(wifiSsid);
  } else {
    Serial.println(F("SSID: (no configurado)"));
  }
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

// ===================== TOKEN TELEGRAM =====================

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
    if (botListo) {
      notificarTodos("LOCKDOWN - Dia 18+. Volteo detenido. Humedad objetivo 65-70%.");
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

// ===================== COMANDOS COMPARTIDOS (SERIAL + BOT) =====================

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
  Serial.println(DIAS_INCUBACION);
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
  Serial.print(F("Proteccion flash: ACTIVA"));
  Serial.println();
  Serial.print(F("WiFi: "));
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");
  Serial.print(F("Bot: "));
  Serial.println(botListo ? "ACTIVO" : "INACTIVO");
}

String obtenerInfoTelegram() {
  String s;
  s += "INCUBADORA AUTOMATICA\n";
  s += "Dia: " + String(diaActual) + "/" + String(DIAS_INCUBACION) + "\n";
  s += "Fase: " + String(enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO") + "\n";
  s += "Temp: " + String(temperatura, 1) + "C (obj " + String(tempObjetivo, 1) + "C)\n";
  s += "Hum: " + String(humedad, 1) + "% (obj " + String(obtenerHumMin(), 0) + "-" + String(obtenerHumMax(), 0) + "%)\n";
  s += "Calefactor: " + String(estadoCalefactor ? "ON" : "OFF") + "\n";
  s += "Humificador: " + String(estadoHumificador ? "ON" : "OFF") + "\n";
  s += "Motor: " + String(motorVolteando ? "GIRANDO" : "OFF") + "\n";
  if (!enLockdown) {
    unsigned long rest = calcularTiempoVolteo();
    s += "Prox. volteo: " + String(rest / 3600) + "h " + String((rest % 3600) / 60) + "m\n";
  }
  s += "Uptime: " + String(obtenerUptimeTotal() / 3600000UL) + "h";
  return s;
}

String obtenerAyudaTelegram() {
  String s;
  s += "COMANDOS DEL BOT:\n";
  s += "info         - estado completo\n";
  s += "+ / -        - subir/bajar temp\n";
  s += "temp 38.0    - fijar temp objetivo\n";
  s += "h            - toggle humificador\n";
  s += "c            - toggle calefactor\n";
  s += "t            - volteo forzado\n";
  s += "s            - guardar estado\n";
  s += "reset si     - borrar estado\n";
  s += "wifi         - estado de la red\n";
  s += "ota <url>    - actualizar firmware\n";
  s += "id           - tu chat_id";
  return s;
}

void mostrarAyudaSerial() {
  Serial.println(F("--- COMANDOS ---"));
  Serial.println(F("+/-           subir/bajar temp"));
  Serial.println(F("temp 38.0     fijar temp objetivo"));
  Serial.println(F("h             toggle humificador"));
  Serial.println(F("c             toggle calefactor"));
  Serial.println(F("t             volteo forzado"));
  Serial.println(F("s             guardar estado"));
  Serial.println(F("r             reset estado"));
  Serial.println(F("d / info      mostrar info"));
  Serial.println(F("wifi          estado de red"));
  Serial.println(F("wifi s c      guardar y conectar (ej: wifi JOCAMER clave)"));
  Serial.println(F("token T       guardar token de Telegram (tambien t=<token>)"));
  Serial.println(F("delwifi       borrar credenciales WiFi"));
  Serial.println(F("deltoken      borrar token del bot"));
  Serial.println(F("allow <id>    permitir chat de Telegram"));
  Serial.println(F("block <id>    bloquear chat de Telegram"));
  Serial.println(F("ota <url>     actualizar firmware (OTA)"));
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
    String arg = "";
    if (sp >= 0) {
      cmd = textoLow.substring(0, sp);
      arg = texto.substring(sp + 1);
      arg.trim();
    }

    if (cmd.length() == 0) continue;

    if (cmd == "start") {
      if (chatsPermitidos.length() == 0) agregarChat(chat);
      if (chatPermitido(chat)) {
        bot.sendMessage(chat, "Hola! Soy el bot de la incubadora.\n\n" + obtenerAyudaTelegram(), "");
      } else {
        bot.sendMessage(chat, "No autorizado. Pide acceso por serial: allow " + chat, "");
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
    } else if (cmd == "+" || cmd == "subir") {
      subirTemp();
      bot.sendMessage(chat, "Temp objetivo: " + String(tempObjetivo, 1) + "C", "");
    } else if (cmd == "-" || cmd == "bajar") {
      bajarTemp();
      bot.sendMessage(chat, "Temp objetivo: " + String(tempObjetivo, 1) + "C", "");
    } else if (cmd == "temp") {
      float v = arg.toFloat();
      if (v > 0) {
        tempObjetivo = v;
        bot.sendMessage(chat, "Temp objetivo: " + String(tempObjetivo, 1) + "C", "");
      } else {
        bot.sendMessage(chat, "Uso: temp 38.0", "");
      }
    } else if (cmd == "h" || cmd == "hum" || cmd == "humificador") {
      toggleHumManual();
      bot.sendMessage(chat, "Humificador manual: " + String(manualHum ? "ON" : "OFF"), "");
    } else if (cmd == "c" || cmd == "cal" || cmd == "calefactor") {
      toggleCalManual();
      bot.sendMessage(chat, "Calefactor manual: " + String(manualCal ? "ON" : "OFF"), "");
    } else if (cmd == "t" || cmd == "vol" || cmd == "volteo") {
      if (enLockdown) {
        bot.sendMessage(chat, "Volteo bloqueado en LOCKDOWN.", "");
      } else {
        forzarVolteoManual();
        bot.sendMessage(chat, "Volteo iniciado.", "");
      }
    } else if (cmd == "s" || cmd == "save" || cmd == "guardar") {
      guardarEstado();
      bot.sendMessage(chat, "Estado guardado.", "");
    } else if (cmd == "reset" || cmd == "r") {
      String argLow = arg;
      argLow.toLowerCase();
      if (argLow == "si" || argLow == "yes") {
        borrarEstado();
        bot.sendMessage(chat, "Estado borrado. Reiniciando.", "");
      } else {
        bot.sendMessage(chat, "Para resetear el estado escribe: reset si", "");
      }
    } else if (cmd == "wifi") {
      bot.sendMessage(chat, "WiFi: " + String(WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado") + " - IP " + WiFi.localIP().toString(), "");
    } else if (cmd == "ota" || cmd == "update") {
      if (arg.length() == 0) {
        bot.sendMessage(chat, "Uso: ota <url_del_bin>", "");
      } else {
        bot.sendMessage(chat, "Descargando e instalando... el equipo reiniciara.", "");
        hacerOTA(arg, chat);
      }
    } else {
      bot.sendMessage(chat, "Comando desconocido. Usa: help", "");
    }
  }
}

void procesarTelegram() {
  if (!botListo) return;

  if (!comandosRegistrados) {
    String cmds = "[{\"command\":\"start\",\"description\":\"Bienvenida\"},{\"command\":\"help\",\"description\":\"Ayuda\"},{\"command\":\"info\",\"description\":\"Estado completo\"},{\"command\":\"temp\",\"description\":\"Fijar temp ej: temp 38\"},{\"command\":\"hum\",\"description\":\"Toggle humificador\"},{\"command\":\"cal\",\"description\":\"Toggle calefactor\"},{\"command\":\"volteo\",\"description\":\"Volteo forzado\"},{\"command\":\"guardar\",\"description\":\"Guardar estado\"},{\"command\":\"ota\",\"description\":\"Actualizar firmware (URL del bin)\"},{\"command\":\"reset\",\"description\":\"Reset estado\"}]";
    if (bot.setMyCommands(cmds)) {
      Serial.println(F("Comandos del bot registrados."));
    }
    comandosRegistrados = true;
  }

  for (int i = 0; i < 5; i++) {
    int num = bot.getUpdates(bot.last_message_received + 1);
    if (num <= 0) break;
    manejarMensajes(num);
  }
}

void verificarAlertas() {
  if (!botListo) return;
  if (temperatura <= 0.0) return;

  bool fuera = (temperatura < TEMP_MIN || temperatura > TEMP_MAX);
  if (fuera && !alarmaTemp) {
    alarmaTemp = true;
    notificarTodos("ALERTA: temperatura fuera de rango: " + String(temperatura, 1) + "C");
  } else if (!fuera && alarmaTemp) {
    alarmaTemp = false;
    notificarTodos("OK: temperatura de nuevo en rango: " + String(temperatura, 1) + "C");
  }
}

// ===================== OTA (ARDUINOOTA + TELEGRAM) =====================

void otaResponder(const String& chat, const String& msg) {
  Serial.println(msg);
  if (chat.length() > 0) bot.sendMessage(chat, msg, "");
}

void hacerOTA(const String& url, const String& chat) {
  if (WiFi.status() != WL_CONNECTED) {
    otaResponder(chat, "Error: sin WiFi.");
    return;
  }

  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);

  WiFiClientSecure otaClient;
  otaClient.setInsecure();
  otaClient.setTimeout(15);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.begin(otaClient, url);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaResponder(chat, "Error HTTP: " + String(code));
    http.end();
    return;
  }

  int len = http.getSize();
  if (len <= 0) {
    otaResponder(chat, "Tamaño del binario desconocido.");
    http.end();
    return;
  }
  if (!Update.begin(len)) {
    otaResponder(chat, "No hay espacio para " + String(len) + " bytes.");
    http.end();
    return;
  }

  Stream &stream = http.getStream();
  uint8_t buf[1024];
  size_t total = 0;
  while (total < (size_t)len) {
    size_t n = stream.readBytes(buf, sizeof(buf));
    if (n == 0) {
      otaResponder(chat, "Conexión cortada: " + String(total) + "/" + String(len) + " bytes.");
      Update.abort();
      http.end();
      return;
    }
    if (Update.write(buf, n) != n) {
      otaResponder(chat, "Fallo de escritura: " + String(Update.getError()));
      Update.abort();
      http.end();
      return;
    }
    total += n;
    yield();
  }

  if (!Update.end(true)) {
    otaResponder(chat, "Fallo final: " + String(Update.getError()));
    http.end();
    return;
  }

  http.end();
  otaResponder(chat, "Actualizado. Reiniciando...");
  delay(500);
  ESP.restart();
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

  // ---- ESTADO RED / BOT ----
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

String serialBuf = "";

String quitarPrefijo(String t) {
  t.trim();
  String low = t;
  low.toLowerCase();
  if (low.startsWith("token=")) return t.substring(7);
  if (low.startsWith("ssid=") || low.startsWith("pass=")) return t.substring(5);
  if (low.startsWith("t=")) return t.substring(2);
  if (low.startsWith("s=") || low.startsWith("p=")) return t.substring(2);
  return t;
}

void procesarLinea(String linea) {
  linea.trim();
  if (linea.length() == 0) return;

  // Comandos de una sola letra (compatibilidad)
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
        Serial.println(F("Uso: wifi <ssid> <clave>  o  wifi s=<ssid> p=<clave>"));
      } else {
        if (pass.length() == 0) {
          Serial.println(F("Sin clave (red abierta)."));
        }
        configurarWifi(ssid, pass);
      }
    }
  } else if (cmdLower == "token") {
    if (arg.length() == 0) {
      Serial.println(telegramToken.length() > 0 ? "Token configurado." : "Token NO configurado.");
    } else {
      String tok = quitarPrefijo(arg);
      if (tok.length() < 10 || tok.indexOf(':') < 0) {
        Serial.println(F("Token invalido. Revisa el token de @BotFather."));
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
      Serial.print(F("Chats permitidos: "));
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
  } else if (cmdLower == "ota") {
    if (arg.length() == 0) {
      Serial.println(F("Uso: ota <url_del_bin>"));
    } else {
      Serial.println(F("Descargando e instalando OTA..."));
      hacerOTA(arg, "");
    }
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

  // Comandos de una sola letra enviados sin salto de linea
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

  // Cargar estado persistente y config de red
  cargarEstado();
  cargarRed();

  // Calcular dia con uptime acumulado
  calcularDia();
  verificarFase();

  // Inicializar timestamps
  ultimoVolteo = obtenerUptimeTotal();
  ultimaLectura = millis();
  ultimaPantalla = millis();
  ultimaGuardado = millis();
  ultimoBot = millis();
  millisEnUltimoGuardado = millis();

  // Bot de Telegram
  bot.updateToken(telegramToken);
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  secured_client.setTimeout(5);
  bot.waitForResponse = 500;

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (wifiSsid.length() > 0) {
    Serial.print(F("Conectando a WiFi: "));
    Serial.println(wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    ultimoIntentoWifi = millis();
  } else {
    Serial.println(F("Sin credenciales WiFi. Usa: wifi <ssid> <password>"));
  }

  if (telegramToken.length() == 0) {
    Serial.println(F("Configura el token del bot: token <BOT_TOKEN>"));
  }

  // OTA
  ArduinoOTA.setHostname("incubadora");
  ArduinoOTA.onStart([]() {
    digitalWrite(PIN_RELAY_HEAT, HIGH);
    digitalWrite(PIN_RELAY_HUM, HIGH);
    digitalWrite(PIN_RELAY_MOTOR, HIGH);
    Serial.println(F("OTA iniciada..."));
  });
  ArduinoOTA.begin();
  Serial.println(F("OTA listo (ArduinoOTA)."));

  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(DIAS_INCUBACION);
  Serial.print(F("Fase: "));
  Serial.println(enLockdown ? "LOCKDOWN" : "DESARROLLO");
  Serial.println(F("Incubadora lista."));
  Serial.println(F("Cmd: +/-:temp temp X h:hum c:cal t:vol s:save d:info r:reset help"));
}

// ===================== LOOP =====================

void loop() {
  unsigned long ahora = millis();

  // Actualizar uptime acumulado
  uptimeAcumulado += (ahora - millisEnUltimoGuardado);
  millisEnUltimoGuardado = ahora;

  // Gestionar WiFi
  gestionarWifi();

  // Estado del bot: WiFi + hora NTP + token
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk && !tiempoSincronizado) {
    time_t now = time(nullptr);
    if (now > 24 * 3600) {
      tiempoSincronizado = true;
      Serial.println(F("Hora NTP sincronizada."));
    }
  }
  botListo = wifiOk && tiempoSincronizado && telegramToken.length() > 0;

  // Leer sensor y calcular fase
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    leerSensor();
    calcularDia();
    verificarFase();
    verificarAlertas();
    ultimaLectura = ahora;
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

  // Consultar bot de Telegram
  if (ahora - ultimoBot >= INTERVALO_BOT) {
    ultimoBot = ahora;
    procesarTelegram();
  }

  // Mantener OTA por red (ArduinoOTA)
  ArduinoOTA.handle();

  // Leer comandos seriales
  procesarSerial();
}
