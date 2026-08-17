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
#include <WebServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
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
const unsigned long INTERVALO_BOT       = 5000UL;
const unsigned long INTERVALO_RECONEXION_WIFI = 30000UL;
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

// ===================== WEB SERVER DASHBOARD =====================
WebServer server(80);

volatile bool webCmdCal = false;
volatile bool webCmdHum = false;
volatile bool webCmdVol = false;
volatile bool webCmdTempUp = false;
volatile bool webCmdTempDown = false;
volatile bool webCmdSave = false;
volatile float webCmdTempSet = 0.0;

const char DASHBOARD_HTML[] PROGMEM =
  "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\"><title>Dashboard Incubadora</title><style>"
  ":root{--bg:#0f1115;--card:#181b21;--text:#e0e0e0;--muted:#888;--green:#00c853;--red:#ff1744;--yellow:#ffd600;--cyan:#00e5ff;--magenta:#e040fb}"
  "*{box-sizing:border-box}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);padding:16px;line-height:1.4}"
  "h1{text-align:center;margin:0 0 20px;font-size:1.6rem;color:var(--yellow)}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}"
  ".card{background:var(--card);border-radius:12px;padding:14px;box-shadow:0 2px 8px rgba(0,0,0,.3)}.card h3{margin:0 0 10px;font-size:.85rem;text-transform:uppercase;color:var(--muted);letter-spacing:.5px}"
  ".value{font-size:2.4rem;font-weight:700;margin:4px 0}.target{font-size:.8rem;color:var(--muted)}.status{margin-top:8px;font-size:.9rem;font-weight:600}"
  ".ok{color:var(--green)}.bad{color:var(--red)}.warn{color:var(--yellow)}.cyan{color:var(--cyan)}.magenta{color:var(--magenta)}.full{grid-column:1/-1}"
  ".progress-bar{width:100%;height:18px;background:#2a2f36;border-radius:9px;overflow:hidden;margin-top:10px}.progress-fill{height:100%;background:var(--green);transition:width .5s ease}"
  ".progress-fill.lockdown{background:var(--magenta)}.controls{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}"
  "button{background:#2a2f36;color:var(--text);border:1px solid #3a4049;border-radius:8px;padding:10px 14px;font-size:.9rem;cursor:pointer;transition:background .2s}"
  "button:hover{background:#3a4049}button:active{transform:scale(.98)}table{width:100%;border-collapse:collapse;font-size:.85rem}"
  "td{padding:6px 0;border-bottom:1px solid #2a2f36}td:first-child{width:35%}code{background:#2a2f36;padding:2px 6px;border-radius:4px;font-family:'SF Mono',monospace;color:var(--cyan)}"
  ".system-info{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:.9rem}@media(max-width:400px){.grid{grid-template-columns:1fr}.system-info{grid-template-columns:1fr}.value{font-size:2rem}}"
  "</style></head><body><h1>Dashboard Incubadora</h1><div class=\"grid\"><div class=\"card\"><h3>Temperatura</h3><div class=\"value\" id=\"temp\">--</div>"
  "<div class=\"target\" id=\"temp-target\">Objetivo: --</div><div class=\"status\" id=\"cal-status\">CAL: --</div></div><div class=\"card\"><h3>Humedad</h3>"
  "<div class=\"value\" id=\"hum\">--</div><div class=\"target\" id=\"hum-target\">Objetivo: --</div><div class=\"status\" id=\"hum-status\">HUM: --</div></div></div>"
  "<div class=\"card full\"><h3>Progreso de Incubación</h3><div>Día <strong id=\"dia\">--</strong>/<strong>21</strong> - <span id=\"fase\" class=\"ok\">--</span></div>"
  "<div class=\"progress-bar\"><div class=\"progress-fill\" id=\"progress\" style=\"width:0%\"></div></div><div class=\"target\" style=\"margin-top:6px\">Uptime: <span id=\"uptime\">--</span></div></div>"
  "<div class=\"card full\"><h3>Volteo</h3><div id=\"volteo\" class=\"cyan\">--</div></div>"
  "<div class=\"card full\"><h3>Control Manual</h3><div class=\"controls\"><button onclick=\"sendCmd('temp_up')\">+ Temp</button>"
  "<button onclick=\"sendCmd('temp_down')\">- Temp</button><button onclick=\"sendCmd('cal')\">Calefactor</button><button onclick=\"sendCmd('hum')\">Humificador</button>"
  "<button onclick=\"sendCmd('vol')\">Forzar Volteo</button><button onclick=\"sendCmd('save')\">Guardar Estado</button></div></div>"
  "<div class=\"card full\"><h3>Comandos del Bot Telegram</h3><table><tr><td><code>info</code></td><td>Estado completo del sistema</td></tr>"
  "<tr><td><code>+</code> / <code>-</code></td><td>Subir / bajar temperatura objetivo</td></tr><tr><td><code>temp 38.0</code></td><td>Fijar temperatura objetivo</td></tr>"
  "<tr><td><code>h</code></td><td>Alternar humidificador manual</td></tr><tr><td><code>c</code></td><td>Alternar calefactor manual</td></tr>"
  "<tr><td><code>t</code></td><td>Volteo forzado</td></tr><tr><td><code>s</code></td><td>Guardar estado en flash</td></tr>"
  "<tr><td><code>reset si</code></td><td>Borrar estado y reiniciar</td></tr><tr><td><code>wifi</code></td><td>Estado de la red WiFi</td></tr>"
  "<tr><td><code>ota &lt;url&gt;</code></td><td>Actualizar firmware por OTA</td></tr><tr><td><code>id</code></td><td>Mostrar tu chat_id</td></tr></table></div>"
  "<div class=\"card full\"><h3>Sistema</h3><div class=\"system-info\"><div>WiFi: <span id=\"wifi\">--</span></div><div>IP: <span id=\"ip\">--</span></div>"
  "<div>Bot Telegram: <span id=\"bot\">--</span></div><div>Última actualización: <span id=\"last-update\">--</span></div></div></div>"
  "<script>const fmtTime=s=>{if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+(s%60)+'s';const h=Math.floor(s/3600);const m=Math.floor((s%3600)/60);return h+'h '+m+'m'};"
  "async function update(){try{const res=await fetch('/api/status');const d=await res.json();document.getElementById('temp').textContent=d.temp.toFixed(1)+'°C';"
  "document.getElementById('temp').className='value '+((d.temp>=37.5&&d.temp<=37.8)?'ok':'bad');document.getElementById('temp-target').textContent='Objetivo: '+d.temp_obj.toFixed(1)+'°C';"
  "const cal=document.getElementById('cal-status');cal.textContent='CAL: '+(d.cal?'ON':'OFF');cal.className='status '+(d.cal?'ok':'bad');"
  "document.getElementById('hum').textContent=d.hum.toFixed(1)+'%';document.getElementById('hum').className='value '+((d.hum>=d.hum_min&&d.hum<=d.hum_max)?'ok':'warn');"
  "document.getElementById('hum-target').textContent='Objetivo: '+d.hum_min.toFixed(0)+'-'+d.hum_max.toFixed(0)+'%';const hum=document.getElementById('hum-status');"
  "hum.textContent='HUM: '+(d.humidor?'ON':'OFF');hum.className='status '+(d.humidor?'ok':'bad');document.getElementById('dia').textContent=d.dia;"
  "document.getElementById('fase').textContent=d.fase;document.getElementById('fase').className=d.lockdown?'magenta':'ok';"
  "document.getElementById('progress').style.width=((d.dia/21)*100)+'%';document.getElementById('progress').className='progress-fill'+(d.lockdown?' lockdown':'');"
  "const vol=document.getElementById('volteo');if(d.lockdown){vol.textContent='LOCKDOWN - Volteo detenido';vol.className='magenta'}else if(d.motor){vol.textContent='GIRANDO';vol.className='warn'}"
  "else{vol.textContent='Próximo volteo: '+fmtTime(d.volteo_restante);vol.className='cyan'}const wifi=document.getElementById('wifi');"
  "wifi.textContent=d.wifi?'Conectado':'Desconectado';wifi.className=d.wifi?'ok':'bad';document.getElementById('ip').textContent=d.ip||'---';"
  "const bot=document.getElementById('bot');bot.textContent=d.bot?'Activo':'Inactivo';bot.className=d.bot?'ok':'bad';"
  "document.getElementById('uptime').textContent=Math.floor(d.uptime/3600)+'h';document.getElementById('last-update').textContent=new Date().toLocaleTimeString()}"
  "catch(e){console.error('Error actualizando:',e);document.getElementById('last-update').textContent='Error de conexión'}}"
  "async function sendCmd(action,value){try{await fetch('/api/control?action='+encodeURIComponent(action)+(value?'&value='+encodeURIComponent(value):''));setTimeout(update,300)}catch(e){alert('Error enviando comando')}}"
  "update();setInterval(update,3000);</script></body></html>";

// ===================== PERSISTENCIA (PROTECCION CORTE) =====================

void guardarEstado() {
  esp_task_wdt_reset();
  preferences.begin("incubadora", false);

  unsigned long uptimeTotal = uptimeAcumulado + millis();
  preferences.putULong("uptime", uptimeTotal);
  preferences.putULong("ultimoVol", ultimoVolteo);
  preferences.putBool("manCal", manualCal);
  preferences.putBool("manHum", manualHum);

  preferences.end();
  esp_task_wdt_reset();
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
  // No hace falta disconnect + begin constante; setAutoReconnect ya intenta reconectar.
  // Forzamos un begin solo si no hay intento de reconexion activo.
  if (WiFi.status() == WL_DISCONNECTED || WiFi.status() == WL_NO_SSID_AVAIL) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
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
  unsigned long t0 = millis();
  auto status = am2302.read();
  unsigned long duracion = millis() - t0;

  if (status == AM2302::AM2302_READ_OK) {
    temperatura = am2302.get_Temperature();
    humedad = am2302.get_Humidity();
    if (duracion > 500) {
      Serial.print(F("Lectura DHT lenta: "));
      Serial.print(duracion);
      Serial.println(F(" ms"));
    }
  } else {
    Serial.print(F("ERROR DHT ("));
    Serial.print(status);
    Serial.print(F("), duracion "));
    Serial.print(duracion);
    Serial.println(F(" ms"));
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
  char buf[512];
  unsigned long uptimeH = obtenerUptimeTotal() / 3600000UL;
  if (!enLockdown) {
    unsigned long rest = calcularTiempoVolteo();
    snprintf(buf, sizeof(buf),
      "INCUBADORA AUTOMATICA\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Prox. volteo: %luh %lum\n"
      "Uptime: %luh",
      diaActual, DIAS_INCUBACION,
      enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      temperatura, tempObjetivo,
      humedad, obtenerHumMin(), obtenerHumMax(),
      estadoCalefactor ? "ON" : "OFF",
      estadoHumificador ? "ON" : "OFF",
      motorVolteando ? "GIRANDO" : "OFF",
      rest / 3600, (rest % 3600) / 60,
      uptimeH);
  } else {
    snprintf(buf, sizeof(buf),
      "INCUBADORA AUTOMATICA\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Uptime: %luh",
      diaActual, DIAS_INCUBACION,
      enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      temperatura, tempObjetivo,
      humedad, obtenerHumMin(), obtenerHumMax(),
      estadoCalefactor ? "ON" : "OFF",
      estadoHumificador ? "ON" : "OFF",
      motorVolteando ? "GIRANDO" : "OFF",
      uptimeH);
  }
  return String(buf);
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

// ===================== WEB SERVER DASHBOARD =====================

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatus() {
  char json[512];
  snprintf(json, sizeof(json),
    "{"
    "\"temp\":%.1f,"
    "\"hum\":%.1f,"
    "\"temp_obj\":%.1f,"
    "\"hum_min\":%.1f,"
    "\"hum_max\":%.1f,"
    "\"cal\":%s,"
    "\"humidor\":%s,"
    "\"motor\":%s,"
    "\"dia\":%d,"
    "\"fase\":\"%s\","
    "\"lockdown\":%s,"
    "\"volteo_restante\":%lu,"
    "\"wifi\":%s,"
    "\"ip\":\"%s\","
    "\"bot\":%s,"
    "\"uptime\":%lu"
    "}",
    temperatura, humedad, tempObjetivo,
    obtenerHumMin(), obtenerHumMax(),
    estadoCalefactor ? "true" : "false",
    estadoHumificador ? "true" : "false",
    motorVolteando ? "true" : "false",
    diaActual,
    enLockdown ? "LOCKDOWN" : "DESARROLLO",
    enLockdown ? "true" : "false",
    calcularTiempoVolteo(),
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "",
    botListo ? "true" : "false",
    obtenerUptimeTotal() / 1000UL);
  server.send(200, "application/json", json);
}

void handleControl() {
  String action = server.arg("action");
  String value = server.arg("value");
  action.toLowerCase();

  if (action == "cal") {
    webCmdCal = true;
  } else if (action == "hum") {
    webCmdHum = true;
  } else if (action == "vol") {
    webCmdVol = true;
  } else if (action == "temp_up") {
    webCmdTempUp = true;
  } else if (action == "temp_down") {
    webCmdTempDown = true;
  } else if (action == "temp_set") {
    webCmdTempSet = value.toFloat();
  } else if (action == "save") {
    webCmdSave = true;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/control", HTTP_GET, handleControl);
  server.begin();
  Serial.println(F("Servidor web iniciado en puerto 80."));
}

void processWebCommands() {
  if (webCmdCal) {
    toggleCalManual();
    webCmdCal = false;
  }
  if (webCmdHum) {
    toggleHumManual();
    webCmdHum = false;
  }
  if (webCmdVol) {
    forzarVolteoManual();
    webCmdVol = false;
  }
  if (webCmdTempUp) {
    subirTemp();
    webCmdTempUp = false;
  }
  if (webCmdTempDown) {
    bajarTemp();
    webCmdTempDown = false;
  }
  if (webCmdTempSet > 0.0) {
    tempObjetivo = webCmdTempSet;
    Serial.print(F("Temp objetivo (web): "));
    Serial.print(tempObjetivo, 1);
    Serial.println(F(" C"));
    webCmdTempSet = 0.0;
  }
  if (webCmdSave) {
    guardarEstado();
    webCmdSave = false;
  }
}

// ===================== BOT TELEGRAM =====================

void notificarTodos(const String& msg) {
  if (!botListo) return;
  String resto = chatsPermitidos;
  while (resto.length() > 0) {
    esp_task_wdt_reset();
    int coma = resto.indexOf(',');
    String id = (coma >= 0) ? resto.substring(0, coma) : resto;
    id.trim();
    if (id.length() > 0) {
      bot.sendMessage(id, msg, "");
      yield();
    }
    if (coma >= 0) resto = resto.substring(coma + 1);
    else break;
  }
}

void manejarMensajes(int num) {
  for (int i = 0; i < num; i++) {
    esp_task_wdt_reset();
    yield();
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
    esp_task_wdt_reset();
    String cmds = "[{\"command\":\"start\",\"description\":\"Bienvenida\"},{\"command\":\"help\",\"description\":\"Ayuda\"},{\"command\":\"info\",\"description\":\"Estado completo\"},{\"command\":\"temp\",\"description\":\"Fijar temp ej: temp 38\"},{\"command\":\"hum\",\"description\":\"Toggle humificador\"},{\"command\":\"cal\",\"description\":\"Toggle calefactor\"},{\"command\":\"volteo\",\"description\":\"Volteo forzado\"},{\"command\":\"guardar\",\"description\":\"Guardar estado\"},{\"command\":\"ota\",\"description\":\"Actualizar firmware (URL del bin)\"},{\"command\":\"reset\",\"description\":\"Reset estado\"}]";
    if (bot.setMyCommands(cmds)) {
      Serial.println(F("Comandos del bot registrados."));
    }
    comandosRegistrados = true;
    yield();
  }

  // Solo un lote de updates por ciclo para no bloquear el loop
  esp_task_wdt_reset();
  int num = bot.getUpdates(bot.last_message_received + 1);
  if (num > 0) {
    manejarMensajes(num);
    yield();
  }
}

void verificarAlertas() {
  esp_task_wdt_reset();
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
  esp_task_wdt_reset();
  if (WiFi.status() != WL_CONNECTED) {
    otaResponder(chat, "Error: sin WiFi.");
    return;
  }

  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);

  WiFiClientSecure otaClient;
  otaClient.setInsecure();
  otaClient.setTimeout(10);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
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
  unsigned long ultimoWdt = millis();
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
    if (millis() - ultimoWdt >= 1000) {
      esp_task_wdt_reset();
      ultimoWdt = millis();
    }
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
  esp_task_wdt_reset();
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

  // Watchdog: 10 segundos; si el loop se bloquea, reinicia
  esp_task_wdt_config_t twdt_config;
  twdt_config.timeout_ms = 10000;
  twdt_config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
  twdt_config.trigger_panic = true;
  if (esp_task_wdt_init(&twdt_config) == ESP_OK) {
    esp_task_wdt_add(NULL);
    Serial.println(F("Watchdog activado (10s)."));
  } else {
    Serial.println(F("Watchdog NO pudo activarse."));
  }

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
  secured_client.setTimeout(3);          // timeout de socket reducido
  bot.waitForResponse = 1500;            // ms de espera de respuesta Telegram

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

  // Web Server Dashboard
  setupWebServer();

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
  esp_task_wdt_reset();
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

  // Procesar comandos del dashboard web
  processWebCommands();

  // Atender peticiones HTTP del dashboard
  server.handleClient();
}
