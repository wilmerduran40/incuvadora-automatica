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
#include <DNSServer.h>
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
constexpr unsigned int PIN_RELAY_FAN   = 33;

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
volatile bool notifyLockdown = false;

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
const unsigned long INTERVALO_LECTURA   = 2000UL;
const unsigned long INTERVALO_PANTALLA  = 1000UL;
const unsigned long INTERVALO_GUARDADO  = 60000UL;
const unsigned long INTERVALO_BOT       = 5000UL;
const unsigned long INTERVALO_RECONEXION_WIFI = 30000UL;
const unsigned long TIMEOUT_AP_WIFI     = 60000UL; // 60s para intentar WiFi antes de activar AP
const unsigned long INTERVALO_HISTORICO = 300000UL; // 5 minutos
const size_t MAX_HISTORICO              = 288;      // 24h a 5 min

// ===================== PERFILES DE INCUBACION =====================
struct Perfil {
  const char* nombre;
  int dias_total;
  int dia_lockdown;
  float temp_objetivo;
  float temp_min;
  float temp_max;
  float hum_min_desarrollo;
  float hum_max_desarrollo;
  float hum_min_lockdown;
  float hum_max_lockdown;
  unsigned long intervalo_volteo_ms;
  unsigned long duracion_volteo_ms;
};

const Perfil PERFILES[] = {
  {"Pollo",        21, 18, 37.6, 37.5, 37.8, 50.0, 55.0, 65.0, 70.0, 2UL*60UL*60UL*1000UL, 20000UL},
  {"Codorniz",     18, 14, 37.5, 37.4, 37.8, 45.0, 50.0, 60.0, 65.0, 2UL*60UL*60UL*1000UL, 20000UL},
  {"Pavo",         28, 25, 37.5, 37.4, 37.8, 50.0, 55.0, 65.0, 70.0, 2UL*60UL*60UL*1000UL, 25000UL},
  {"Pato",         28, 25, 37.5, 37.4, 37.8, 55.0, 60.0, 70.0, 75.0, 2UL*60UL*60UL*1000UL, 25000UL},
  {"Personalizado",21, 18, 37.6, 37.5, 37.8, 50.0, 55.0, 65.0, 70.0, 2UL*60UL*60UL*1000UL, 20000UL}
};
const int NUM_PERFILES = sizeof(PERFILES) / sizeof(PERFILES[0]);

Perfil perfilActivo = PERFILES[0];
int perfilIdActivo = 0;

// Parámetros editables (copia del perfil activo, usado también para personalizado)
float tempObjetivo = 37.5;
float tempMin      = 37.5;
float tempMax      = 37.8;

// ===================== VARIABLES DE ESTADO =====================
float temperatura = 0.0;
float humedad = 0.0;

bool estadoCalefactor = false;
bool estadoHumificador = false;
bool motorVolteando = false;
bool enLockdown = false;
bool estadoVentilador = true;
bool ventiladorManual = false;

unsigned long ultimoVolteo = 0;
unsigned long inicioVolteo = 0;
unsigned long ultimaLectura = 0;
unsigned long ultimaPantalla = 0;
unsigned long ultimaGuardado = 0;
unsigned long ultimoBot = 0;
unsigned long inicioIncubacion = 0;
unsigned long uptimeAcumulado = 0;
unsigned long millisEnUltimoGuardado = 0;
unsigned long ultimoMuestreoHistorico = 0;

int diaActual = 0;

bool manualCal = false;
bool manualHum = false;

// ===================== DISPLAY / SENSOR INIT =====================
bool displayOk = false;
bool sensorCalentando = true;
int sensorWarmupCount = 0;
bool actuadoresListos = false;
unsigned long millisArranque = 0;
const unsigned long DELAY_ACTUADORES_MS = 500UL; // evita brown-out al boot con modulo relay
const unsigned long STAGGER_RELAY_MS = 50UL;      // no activar varias bobinas a la vez
unsigned long ultimoCambioRelay = 0;

// ===================== HISTORICO PARA GRAFICOS =====================
struct MuestraHistorico {
  unsigned long uptime;
  float temp;
  float hum;
};
MuestraHistorico historico[MAX_HISTORICO];
size_t historicoCount = 0;
size_t historicoIndex = 0;

// ===================== MODO AP / CONFIGURACION =====================
bool modoAP = false;
bool intentoWiFiInicial = false;
unsigned long inicioIntentoWiFi = 0;
DNSServer dnsServer;
const char* AP_SSID = "Incubadora-Setup";
const char* AP_PASS = "incubadora123";

// ===================== WEB SERVER DASHBOARD =====================
WebServer server(80);

volatile bool webCmdCal = false;
volatile bool webCmdHum = false;
volatile bool webCmdVol = false;
volatile bool webCmdTempUp = false;
volatile bool webCmdTempDown = false;
volatile bool webCmdSave = false;
volatile float webCmdTempSet = 0.0;
volatile bool webCmdFan = false;
volatile int webCmdProfileId = -1;
volatile bool webCmdCustom = false;
volatile float webCmdCustomParams[10] = {0};

const char DASHBOARD_HTML[] PROGMEM =
  "<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>Dashboard Incubadora</title><style> :root { --bg: #0f1115; --card: #181b21; --text: #e0e0e0; --muted: #888; --green: #00c853; --red: #ff1744; --yellow: #ffd600; --cyan: #00e5ff; --magenta: #e040fb; --blue: #2979ff; --orange: #ff9100; } * { box-sizing: border-box; } body { margin: 0; font-family: -apple-system, BlinkMacSystemFont, \'Segoe UI\', Roboto, sans-serif; background: var(--bg); color: var(--text); padding: 16px; line-height: 1.4; } h1 { text-align: center; margin: 0 0 16px; font-size: 1.5rem; color: var(--yellow); } .tabs { display: flex; gap: 8px; margin-bottom: 16px; border-bottom: 1px solid #2a2f36; padding-bottom: 8px; } .tab-btn { flex: 1; background: transparent; border: none; color: var(--muted); padding: 10px; font-size: 0.95rem; cursor: pointer; border-radius: 8px; transition: all 0.2s; } .tab-btn.active { background: var(--card); color: var(--text); } .tab-content { display: none; } .tab-content.active { display: block; } .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 12px; } .card { background: var(--card); border-radius: 12px; padding: 14px; box-shadow: 0 2px 8px rgba(0,0,0,0.3); margin-bottom: 12px; } .card h3 { margin: 0 0 10px; font-size: 0.8rem; text-transform: uppercase; color: var(--muted); letter-spacing: 0.5px; } .value { font-size: 2.2rem; font-weight: 700; margin: 4px 0; } .target { font-size: 0.8rem; color: var(--muted); } .status { margin-top: 8px; font-size: 0.9rem; font-weight: 600; } .ok { color: var(--green); } .bad { color: var(--red); } .warn { color: var(--yellow); } .cyan { color: var(--cyan); } .magenta { color: var(--magenta); } .blue { color: var(--blue); } .orange { color: var(--orange); } .full { grid-column: 1 / -1; } .progress-bar { width: 100%; height: 18px; background: #2a2f36; border-radius: 9px; overflow: hidden; margin-top: 10px; } .progress-fill { height: 100%; background: var(--green); transition: width 0.5s ease; } .progress-fill.lockdown { background: var(--magenta); } .controls { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 8px; } button, input[type=submit] { background: #2a2f36; color: var(--text); border: 1px solid #3a4049; border-radius: 8px; padding: 10px 14px; font-size: 0.9rem; cursor: pointer; transition: background 0.2s; } button:hover, input[type=submit]:hover { background: #3a4049; } button:active { transform: scale(0.98); } button.on { background: var(--green); color: #000; border-color: var(--green); } button.off { background: var(--red); color: #fff; border-color: var(--red); } .form-group { margin-bottom: 12px; } label { display: block; font-size: 0.85rem; color: var(--muted); margin-bottom: 4px; } input, select { width: 100%; background: #2a2f36; color: var(--text); border: 1px solid #3a4049; border-radius: 8px; padding: 10px; font-size: 0.95rem; } .system-info { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; font-size: 0.9rem; } .toast { position: fixed; bottom: 16px; left: 50%; transform: translateX(-50%); background: var(--card); color: var(--text); padding: 10px 18px; border-radius: 8px; border: 1px solid #3a4049; display: none; z-index: 100; } .chart-container { width: 100%; height: 260px; background: #0f1115; border-radius: 12px; padding: 10px; } svg { width: 100%; height: 100%; } .legend { display: flex; gap: 16px; font-size: 0.85rem; margin-bottom: 10px; } .legend span::before { content: \'\'; display: inline-block; width: 12px; height: 12px; border-radius: 2px; margin-right: 4px; } .legend .temp::before { background: var(--red); } .legend .hum::before { background: var(--cyan); } @media (max-width: 400px) { .grid { grid-template-columns: 1fr; } .system-info { grid-template-columns: 1fr; } .value { font-size: 2rem; } } </style></head><body><h1>Dashboard Incubadora</h1><div class=\"tabs\"><button class=\"tab-btn active\" onclick=\"showTab(\'estado\')\">Estado</button><button class=\"tab-btn\" onclick=\"showTab(\'graficos\')\">Gráficos</button><button class=\"tab-btn\" onclick=\"showTab(\'config\')\">Configuración</button></div><div id=\"estado\" class=\"tab-content active\"><div class=\"card full\"><h3>Perfil de Incubación</h3><div class=\"controls\"><select id=\"perfil-select\" onchange=\"cambiarPerfil(this.value)\"><option value=\"0\">Pollo</option><option value=\"1\">Codorniz</option><option value=\"2\">Pavo</option><option value=\"3\">Pato</option><option value=\"4\">Personalizado</option></select></div><div class=\"target\" style=\"margin-top:8px\">Especies: <span id=\"perfil-nombre\">--</span></div></div><div class=\"grid\"><div class=\"card\"><h3>Temperatura</h3><div class=\"value\" id=\"temp\">--</div><div class=\"target\" id=\"temp-target\">Objetivo: --</div><div class=\"status\" id=\"cal-status\">CAL: --</div></div><div class=\"card\"><h3>Humedad</h3><div class=\"value\" id=\"hum\">--</div><div class=\"target\" id=\"hum-target\">Objetivo: --</div><div class=\"status\" id=\"hum-status\">HUM: --</div></div><div class=\"card\"><h3>Ventilador</h3><div class=\"value cyan\" id=\"fan\">--</div><div class=\"target\">Recirculación de aire</div><div class=\"controls\"><button id=\"fan-btn\" onclick=\"sendCmd(\'fan\')\">--</button></div></div><div class=\"card\"><h3>Sistema</h3><div class=\"system-info\"><div>WiFi: <span id=\"wifi\">--</span></div><div>IP: <span id=\"ip\">--</span></div><div>Bot: <span id=\"bot\">--</span></div><div>Modo: <span id=\"modo\">--</span></div></div></div></div><div class=\"card full\"><h3>Progreso de Incubación</h3><div> Día <strong id=\"dia\">--</strong>/<strong id=\"dias-total\">--</strong> - <span id=\"fase\" class=\"ok\">--</span></div><div class=\"progress-bar\"><div class=\"progress-fill\" id=\"progress\" style=\"width:0%\"></div></div><div class=\"target\" style=\"margin-top:6px\">Uptime: <span id=\"uptime\">--</span></div></div><div class=\"card full\"><h3>Volteo</h3><div id=\"volteo\" class=\"cyan\">--</div></div><div class=\"card full\"><h3>Control Manual</h3><div class=\"controls\"><button onclick=\"sendCmd(\'temp_up\')\">+ Temp</button><button onclick=\"sendCmd(\'temp_down\')\">- Temp</button><button onclick=\"sendCmd(\'cal\')\">Calefactor</button><button onclick=\"sendCmd(\'hum\')\">Humificador</button><button onclick=\"sendCmd(\'vol\')\">Forzar Volteo</button><button onclick=\"sendCmd(\'save\')\">Guardar Estado</button></div></div><div class=\"card full\"><h3>Última actualización</h3><div class=\"target\" id=\"last-update\">--</div></div></div><div id=\"graficos\" class=\"tab-content\"><div class=\"card full\"><h3>Histórico (últimas 24h)</h3><div class=\"legend\"><span class=\"temp\">Temperatura (°C)</span><span class=\"hum\">Humedad (%)</span></div><div class=\"chart-container\"><svg id=\"chart\" viewBox=\"0 0 600 240\" preserveAspectRatio=\"none\"><g id=\"chart-grid\"></g><path id=\"chart-temp\" fill=\"none\" stroke=\"#ff1744\" stroke-width=\"2\"></path><path id=\"chart-hum\" fill=\"none\" stroke=\"#00e5ff\" stroke-width=\"2\"></path></svg></div></div></div><div id=\"config\" class=\"tab-content\"><div class=\"card full\"><h3>Configuración WiFi</h3><form onsubmit=\"guardarConfig(event)\"><div class=\"form-group\"><label>SSID</label><input type=\"text\" id=\"cfg-ssid\" placeholder=\"Nombre de red\" required></div><div class=\"form-group\"><label>Contraseña</label><input type=\"password\" id=\"cfg-pass\" placeholder=\"Contraseña\"></div><div class=\"form-group\"><label>Token Bot Telegram</label><input type=\"text\" id=\"cfg-token\" placeholder=\"Opcional\"></div><div class=\"form-group\"><label>Chat IDs permitidos (separados por coma)</label><input type=\"text\" id=\"cfg-chats\" placeholder=\"Opcional\"></div><input type=\"submit\" value=\"Guardar y reiniciar\"></form></div><div class=\"card full\" id=\"custom-panel\" style=\"display:none\"><h3>Parámetros Personalizados</h3><form onsubmit=\"guardarCustom(event)\"><div class=\"grid\"><div class=\"form-group\"><label>Días totales</label><input type=\"number\" id=\"c-dias\" value=\"21\" required></div><div class=\"form-group\"><label>Día lockdown</label><input type=\"number\" id=\"c-lockdown\" value=\"18\" required></div><div class=\"form-group\"><label>Temp objetivo</label><input type=\"number\" step=\"0.1\" id=\"c-temp-obj\" value=\"37.6\" required></div><div class=\"form-group\"><label>Temp mínima</label><input type=\"number\" step=\"0.1\" id=\"c-temp-min\" value=\"37.5\" required></div><div class=\"form-group\"><label>Temp máxima</label><input type=\"number\" step=\"0.1\" id=\"c-temp-max\" value=\"37.8\" required></div><div class=\"form-group\"><label>Hum mín desarrollo</label><input type=\"number\" step=\"0.1\" id=\"c-hum-min-dev\" value=\"50\" required></div><div class=\"form-group\"><label>Hum máx desarrollo</label><input type=\"number\" step=\"0.1\" id=\"c-hum-max-dev\" value=\"55\" required></div><div class=\"form-group\"><label>Hum mín lockdown</label><input type=\"number\" step=\"0.1\" id=\"c-hum-min-lock\" value=\"65\" required></div><div class=\"form-group\"><label>Hum máx lockdown</label><input type=\"number\" step=\"0.1\" id=\"c-hum-max-lock\" value=\"70\" required></div><div class=\"form-group\"><label>Intervalo volteo (horas)</label><input type=\"number\" step=\"0.5\" id=\"c-volteo\" value=\"2\" required></div></div><input type=\"submit\" value=\"Aplicar personalizado\"></form></div></div><div class=\"toast\" id=\"toast\"></div><script> let currentData = {}; const fmtTime = s => { if (s < 60) return s + \'s\'; if (s < 3600) return Math.floor(s/60) + \'m \' + (s%60) + \'s\'; const h = Math.floor(s/3600); const m = Math.floor((s%3600)/60); return h + \'h \' + m + \'m\'; }; function showTab(id) { document.querySelectorAll(\'.tab-content\').forEach(el => el.classList.remove(\'active\')); document.querySelectorAll(\'.tab-btn\').forEach(el => el.classList.remove(\'active\')); document.getElementById(id).classList.add(\'active\'); event.target.classList.add(\'active\'); if (id === \'graficos\') cargarHistorico(); } function toast(msg) { const t = document.getElementById(\'toast\'); t.textContent = msg; t.style.display = \'block\'; setTimeout(() => t.style.display = \'none\', 2500); } async function update() { try { const res = await fetch(\'/api/status\'); const d = await res.json(); currentData = d; document.getElementById(\'temp\').textContent = d.temp.toFixed(1) + \'°C\'; document.getElementById(\'temp\').className = \'value \' + ((d.temp >= d.temp_min && d.temp <= d.temp_max) ? \'ok\' : \'bad\'); document.getElementById(\'temp-target\').textContent = \'Objetivo: \' + d.temp_obj.toFixed(1) + \'°C (\' + d.temp_min.toFixed(1) + \'-\' + d.temp_max.toFixed(1) + \')\'; const cal = document.getElementById(\'cal-status\'); cal.textContent = \'CAL: \' + (d.cal ? \'ON\' : \'OFF\'); cal.className = \'status \' + (d.cal ? \'ok\' : \'bad\'); document.getElementById(\'hum\').textContent = d.hum.toFixed(1) + \'%\'; document.getElementById(\'hum\').className = \'value \' + ((d.hum >= d.hum_min && d.hum <= d.hum_max) ? \'ok\' : \'warn\'); document.getElementById(\'hum-target\').textContent = \'Objetivo: \' + d.hum_min.toFixed(0) + \'-\' + d.hum_max.toFixed(0) + \'%\'; const hum = document.getElementById(\'hum-status\'); hum.textContent = \'HUM: \' + (d.humidor ? \'ON\' : \'OFF\'); hum.className = \'status \' + (d.humidor ? \'ok\' : \'bad\'); document.getElementById(\'fan\').textContent = d.fan ? \'ON\' : \'OFF\'; document.getElementById(\'fan\').className = \'value \' + (d.fan ? \'cyan\' : \'bad\'); const fanBtn = document.getElementById(\'fan-btn\'); fanBtn.textContent = d.fan ? \'Apagar ventilador\' : \'Encender ventilador\'; fanBtn.className = d.fan ? \'on\' : \'off\'; document.getElementById(\'perfil-select\').value = d.perfil_id; document.getElementById(\'perfil-nombre\').textContent = d.perfil; document.getElementById(\'custom-panel\').style.display = (d.perfil_id == 4) ? \'block\' : \'none\'; document.getElementById(\'dia\').textContent = d.dia; document.getElementById(\'dias-total\').textContent = d.dias_total; document.getElementById(\'fase\').textContent = d.fase; document.getElementById(\'fase\').className = d.lockdown ? \'magenta\' : \'ok\'; document.getElementById(\'progress\').style.width = ((d.dia / d.dias_total) * 100) + \'%\'; document.getElementById(\'progress\').className = \'progress-fill\' + (d.lockdown ? \' lockdown\' : \'\'); const vol = document.getElementById(\'volteo\'); if (d.lockdown) { vol.textContent = \'LOCKDOWN - Volteo detenido\'; vol.className = \'magenta\'; } else if (d.motor) { vol.textContent = \'GIRANDO\'; vol.className = \'warn\'; } else { vol.textContent = \'Próximo volteo: \' + fmtTime(d.volteo_restante); vol.className = \'cyan\'; } const wifi = document.getElementById(\'wifi\'); wifi.textContent = d.wifi ? \'Conectado\' : \'Desconectado\'; wifi.className = d.wifi ? \'ok\' : \'bad\'; document.getElementById(\'ip\').textContent = d.ip || \'---\'; const bot = document.getElementById(\'bot\'); bot.textContent = d.bot ? \'Activo\' : \'Inactivo\'; bot.className = d.bot ? \'ok\' : \'bad\'; const modo = document.getElementById(\'modo\'); modo.textContent = d.modo_ap ? \'AP\' : \'STA\'; modo.className = d.modo_ap ? \'warn\' : \'cyan\'; document.getElementById(\'uptime\').textContent = Math.floor(d.uptime / 3600) + \'h\'; document.getElementById(\'last-update\').textContent = new Date().toLocaleTimeString(); } catch (e) { console.error(\'Error actualizando:\', e); document.getElementById(\'last-update\').textContent = \'Error de conexión\'; } } async function sendCmd(action, value) { try { await fetch(\'/api/control?action=\' + encodeURIComponent(action) + (value ? \'&value=\' + encodeURIComponent(value) : \'\')); toast(\'Comando enviado\'); setTimeout(update, 300); } catch (e) { toast(\'Error enviando comando\'); } } async function cambiarPerfil(id) { await sendCmd(\'profile\', id); } async function guardarConfig(e) { e.preventDefault(); const ssid = document.getElementById(\'cfg-ssid\').value; const pass = document.getElementById(\'cfg-pass\').value; const token = document.getElementById(\'cfg-token\').value; const chats = document.getElementById(\'cfg-chats\').value; try { const res = await fetch(\'/api/config\', { method: \'POST\', headers: {\'Content-Type\': \'application/x-www-form-urlencoded\'}, body: \'ssid=\' + encodeURIComponent(ssid) + \'&pass=\' + encodeURIComponent(pass) + \'&token=\' + encodeURIComponent(token) + \'&chats=\' + encodeURIComponent(chats) }); const d = await res.json(); toast(d.msg); } catch (e) { toast(\'Error guardando configuración\'); } } async function guardarCustom(e) { e.preventDefault(); const params = { dias_total: document.getElementById(\'c-dias\').value, dia_lockdown: document.getElementById(\'c-lockdown\').value, temp_obj: document.getElementById(\'c-temp-obj\').value, temp_min: document.getElementById(\'c-temp-min\').value, temp_max: document.getElementById(\'c-temp-max\').value, hum_min_dev: document.getElementById(\'c-hum-min-dev\').value, hum_max_dev: document.getElementById(\'c-hum-max-dev\').value, hum_min_lock: document.getElementById(\'c-hum-min-lock\').value, hum_max_lock: document.getElementById(\'c-hum-max-lock\').value, intervalo_volteo_h: document.getElementById(\'c-volteo\').value }; let qs = \'action=custom\'; for (let k in params) qs += \'&\' + k + \'=\' + encodeURIComponent(params[k]); try { await fetch(\'/api/control?\' + qs); toast(\'Personalizado aplicado\'); setTimeout(update, 300); } catch (e) { toast(\'Error aplicando personalizado\'); } } async function cargarHistorico() { try { const res = await fetch(\'/api/history\'); const data = await res.json(); dibujarGrafico(data); } catch (e) { console.error(\'Error cargando histórico:\', e); } } function dibujarGrafico(data) { if (data.length < 2) return; const svg = document.getElementById(\'chart\'); const w = 600, h = 240, pad = 30; const gw = w - pad * 2, gh = h - pad * 2; let minT = 30, maxT = 45, minH = 20, maxH = 90; data.forEach(p => { if (p.temp < minT) minT = p.temp; if (p.temp > maxT) maxT = p.temp; if (p.hum < minH) minH = p.hum; if (p.hum > maxH) maxH = p.hum; }); minT = Math.floor(minT); maxT = Math.ceil(maxT); minH = Math.floor(minH / 10) * 10; maxH = Math.ceil(maxH / 10) * 10; const t0 = data[0].t, tn = data[data.length - 1].t; const dx = tn === t0 ? 0 : gw / (tn - t0); const pt = (x, y, min, max) => { const px = pad + (x - t0) * dx; const py = pad + gh - ((y - min) / (max - min)) * gh; return px.toFixed(1) + \',\' + py.toFixed(1); }; document.getElementById(\'chart-temp\').setAttribute(\'d\', \'M \' + data.map(p => pt(p.t, p.temp, minT, maxT)).join(\' L \')); document.getElementById(\'chart-hum\').setAttribute(\'d\', \'M \' + data.map(p => pt(p.t, p.hum, minH, maxH)).join(\' L \')); const grid = document.getElementById(\'chart-grid\'); grid.innerHTML = \'\'; for (let i = 0; i <= 4; i++) { const y = pad + (gh * i) / 4; grid.innerHTML += `<line x1=\"${pad}\" y1=\"${y}\" x2=\"${w-pad}\" y2=\"${y}\" stroke=\"#2a2f36\" stroke-width=\"1\"/>`; grid.innerHTML += `<text x=\"5\" y=\"${y+4}\" fill=\"#888\" font-size=\"10\">${(maxT - (maxT-minT)*i/4).toFixed(1)}</text>`; grid.innerHTML += `<text x=\"${w-pad+4}\" y=\"${y+4}\" fill=\"#888\" font-size=\"10\">${(maxH - (maxH-minH)*i/4).toFixed(0)}</text>`; } } update(); setInterval(update, 3000); </script></body></html> ";

// ===================== TAREA TELEGRAM (CORE 0) =====================
// La tarea de Telegram corre en core 0 para no bloquear el loop principal (core 1).
// Si Telegram se bloquea por red, el control de actuadores sigue funcionando.

enum TelegramCmdType {
  TCMD_NONE = 0,
  TCMD_SUBIR_TEMP,
  TCMD_BAJAR_TEMP,
  TCMD_SET_TEMP,
  TCMD_TOGGLE_HUM,
  TCMD_TOGGLE_CAL,
  TCMD_FORZAR_VOLTEO,
  TCMD_SAVE,
  TCMD_RESET,
  TCMD_APLICAR_PERFIL,
  TCMD_TOGGLE_VENTILADOR
};

struct TelegramCmdMsg {
  TelegramCmdType cmd;
  float value;
};

QueueHandle_t telegramCmdQueue = NULL;
SemaphoreHandle_t stateMutex = NULL;

struct StateSnapshot {
  float temperatura;
  float humedad;
  float tempObjetivo;
  float tempMin;
  float tempMax;
  float humMin;
  float humMax;
  bool estadoCalefactor;
  bool estadoHumificador;
  bool motorVolteando;
  bool estadoVentilador;
  bool enLockdown;
  int diaActual;
  int perfilDiasTotal;
  int perfilIdActivo;
  const char* perfilNombre;
  unsigned long uptime;
  unsigned long tiempoVolteoRestante;
};

StateSnapshot snap;
TaskHandle_t telegramTaskHandle = NULL;

// ===================== PERSISTENCIA (PROTECCION CORTE) =====================

void guardarEstado() {
  preferences.begin("incubadora", false);

  unsigned long uptimeTotal = uptimeAcumulado + millis();
  preferences.putULong("uptime", uptimeTotal);
  preferences.putULong("ultimoVol", ultimoVolteo);
  preferences.putBool("manCal", manualCal);
  preferences.putBool("manHum", manualHum);

  // Guardar perfil activo y parámetros personalizados
  preferences.putInt("perfilId", perfilIdActivo);
  preferences.putInt("p_dias_total", perfilActivo.dias_total);
  preferences.putInt("p_dia_lockdown", perfilActivo.dia_lockdown);
  preferences.putFloat("p_temp_obj", perfilActivo.temp_objetivo);
  preferences.putFloat("p_temp_min", perfilActivo.temp_min);
  preferences.putFloat("p_temp_max", perfilActivo.temp_max);
  preferences.putFloat("p_hum_min_dev", perfilActivo.hum_min_desarrollo);
  preferences.putFloat("p_hum_max_dev", perfilActivo.hum_max_desarrollo);
  preferences.putFloat("p_hum_min_lock", perfilActivo.hum_min_lockdown);
  preferences.putFloat("p_hum_max_lock", perfilActivo.hum_max_lockdown);
  preferences.putULong("p_int_volteo", perfilActivo.intervalo_volteo_ms);
  preferences.putULong("p_dur_volteo", perfilActivo.duracion_volteo_ms);
  preferences.putBool("fan_on", estadoVentilador);

  preferences.end();
  Serial.println(F("Estado guardado."));
}

void cargarEstado() {
  preferences.begin("incubadora", true);

  unsigned long uptimeGuardado = preferences.getULong("uptime", 0);
  ultimoVolteo = preferences.getULong("ultimoVol", 0);
  manualCal = preferences.getBool("manCal", false);
  manualHum = preferences.getBool("manHum", false);

  perfilIdActivo = preferences.getInt("perfilId", 0);
  if (perfilIdActivo < 0 || perfilIdActivo >= NUM_PERFILES) perfilIdActivo = 0;

  // Cargar parámetros personalizados si existen; si no, usar valores por defecto
  Perfil base = PERFILES[perfilIdActivo];
  perfilActivo.nombre = base.nombre;
  perfilActivo.dias_total = preferences.getInt("p_dias_total", base.dias_total);
  perfilActivo.dia_lockdown = preferences.getInt("p_dia_lockdown", base.dia_lockdown);
  perfilActivo.temp_objetivo = preferences.getFloat("p_temp_obj", base.temp_objetivo);
  perfilActivo.temp_min = preferences.getFloat("p_temp_min", base.temp_min);
  perfilActivo.temp_max = preferences.getFloat("p_temp_max", base.temp_max);
  perfilActivo.hum_min_desarrollo = preferences.getFloat("p_hum_min_dev", base.hum_min_desarrollo);
  perfilActivo.hum_max_desarrollo = preferences.getFloat("p_hum_max_dev", base.hum_max_desarrollo);
  perfilActivo.hum_min_lockdown = preferences.getFloat("p_hum_min_lock", base.hum_min_lockdown);
  perfilActivo.hum_max_lockdown = preferences.getFloat("p_hum_max_lock", base.hum_max_lockdown);
  perfilActivo.intervalo_volteo_ms = preferences.getULong("p_int_volteo", base.intervalo_volteo_ms);
  perfilActivo.duracion_volteo_ms = preferences.getULong("p_dur_volteo", base.duracion_volteo_ms);
  estadoVentilador = preferences.getBool("fan_on", true);

  preferences.end();

  uptimeAcumulado = uptimeGuardado;
  millisEnUltimoGuardado = millis();

  // Aplicar parámetros editables
  tempObjetivo = perfilActivo.temp_objetivo;
  tempMin = perfilActivo.temp_min;
  tempMax = perfilActivo.temp_max;
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
  perfilIdActivo = 0;
  perfilActivo = PERFILES[0];
  tempObjetivo = perfilActivo.temp_objetivo;
  tempMin = perfilActivo.temp_min;
  tempMax = perfilActivo.temp_max;
  estadoVentilador = true;

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
  inicioIntentoWiFi = millis();
  intentoWiFiInicial = true;
}

void iniciarModoAP() {
  if (modoAP) return;
  Serial.println(F("Iniciando modo AP..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  modoAP = true;
  Serial.print(F("AP: "));
  Serial.println(AP_SSID);
  Serial.print(F("IP: "));
  Serial.println(WiFi.softAPIP());
}

void detenerModoAP() {
  if (!modoAP) return;
  Serial.println(F("Cerrando modo AP..."));
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  modoAP = false;
}

void gestionarWifi() {
  static bool ntpArrancado = false;

  if (modoAP) {
    dnsServer.processNextRequest();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpArrancado) {
      configTime(0, 0, "pool.ntp.org");
      ntpArrancado = true;
    }
    ultimoIntentoWifi = millis();
    inicioIntentoWiFi = millis();
    return;
  }

  // Si no hay credenciales, entrar directo en modo AP
  if (wifiSsid.length() == 0) {
    iniciarModoAP();
    return;
  }

  // Primer intento: esperar TIMEOUT_AP_WIFI antes de activar AP
  if (!intentoWiFiInicial) {
    inicioIntentoWiFi = millis();
    intentoWiFiInicial = true;
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.print(F("Conectando WiFi: "));
    Serial.println(wifiSsid);
    return;
  }

  if (millis() - inicioIntentoWiFi >= TIMEOUT_AP_WIFI) {
    Serial.println(F("No se pudo conectar. Activando modo AP."));
    iniciarModoAP();
    return;
  }

  // Reintentos periódicos
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
  if (diaActual > perfilActivo.dias_total) {
    diaActual = perfilActivo.dias_total;
  }
}

void verificarFase() {
  bool eraLockdown = enLockdown;
  enLockdown = (diaActual >= perfilActivo.dia_lockdown);

  if (enLockdown && !eraLockdown) {
    Serial.println(F("*** LOCKDOWN - Dia 18+ ***"));
    Serial.println(F("Volteo DETENIDO. Humedad: 65-70%"));
    if (motorVolteando) {
      detenerVolteo();
    }
    // Flag para que la tarea de Telegram envíe la notificación (sin bloquear core 1)
    notifyLockdown = true;
  }
}

float obtenerHumMin() {
  return enLockdown ? perfilActivo.hum_min_lockdown : perfilActivo.hum_min_desarrollo;
}

float obtenerHumMax() {
  return enLockdown ? perfilActivo.hum_max_lockdown : perfilActivo.hum_max_desarrollo;
}

// ===================== CONTROL DE ACTUADORES =====================

// Relays activos en LOW. HIGH antes de pinMode evita pico de corriente al boot.
void relaysInitSeguro() {
  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  digitalWrite(PIN_RELAY_FAN, HIGH);
  pinMode(PIN_RELAY_HEAT, OUTPUT);
  pinMode(PIN_RELAY_HUM, OUTPUT);
  pinMode(PIN_RELAY_MOTOR, OUTPUT);
  pinMode(PIN_RELAY_FAN, OUTPUT);
  digitalWrite(PIN_RELAY_HEAT, HIGH);
  digitalWrite(PIN_RELAY_HUM, HIGH);
  digitalWrite(PIN_RELAY_MOTOR, HIGH);
  digitalWrite(PIN_RELAY_FAN, HIGH);
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

void controlarCalefactor() {
  if (!actuadoresListos || manualCal) return;

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
  if (!actuadoresListos) return;

  float humMin = obtenerHumMin();
  float humMax = obtenerHumMax();

  if (manualHum) {
    if (humedad < humMin && !estadoHumificador) {
      if (!relayPuedeCambiar()) return;
      digitalWrite(PIN_RELAY_HUM, LOW);
      estadoHumificador = true;
    } else if (humedad > humMax && estadoHumificador) {
      if (!relayPuedeCambiar()) return;
      digitalWrite(PIN_RELAY_HUM, HIGH);
      estadoHumificador = false;
    }
    return;
  }

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
  if (!actuadoresListos || motorVolteando || enLockdown) return;
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

void controlarVentilador() {
  if (!actuadoresListos || ventiladorManual) return;
  // Reafirma estado sin stagger: si el pin ya esta asi, no hay pico de corriente
  digitalWrite(PIN_RELAY_FAN, estadoVentilador ? LOW : HIGH);
}

void encenderVentilador() {
  estadoVentilador = true;
  ventiladorManual = false;
  if (!actuadoresListos) return;
  digitalWrite(PIN_RELAY_FAN, LOW);
  ultimoCambioRelay = millis();
}

void apagarVentilador() {
  estadoVentilador = false;
  ventiladorManual = true;
  if (!actuadoresListos) return;
  digitalWrite(PIN_RELAY_FAN, HIGH);
  ultimoCambioRelay = millis();
}

void toggleVentilador() {
  if (estadoVentilador) {
    apagarVentilador();
  } else {
    encenderVentilador();
  }
}

void verificarVolteo(unsigned long ahora) {
  if (enLockdown) return;

  if (motorVolteando) {
    if (ahora - inicioVolteo >= perfilActivo.duracion_volteo_ms) {
      detenerVolteo();
    }
    return;
  }

  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }

  if (obtenerUptimeTotal() - tv >= perfilActivo.intervalo_volteo_ms) {
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

void guardarMuestraHistorico(unsigned long ahora) {
  if (temperatura <= 0.0 && humedad <= 0.0) return;
  if (ahora - ultimoMuestreoHistorico < INTERVALO_HISTORICO) return;
  ultimoMuestreoHistorico = ahora;

  historico[historicoIndex].uptime = obtenerUptimeTotal() / 1000UL;
  historico[historicoIndex].temp = temperatura;
  historico[historicoIndex].hum = humedad;
  historicoIndex = (historicoIndex + 1) % MAX_HISTORICO;
  if (historicoCount < MAX_HISTORICO) historicoCount++;
}

unsigned long calcularTiempoVolteo() {
  if (enLockdown) return 0;
  unsigned long tv = ultimoVolteo;
  if (tv == 0 || tv > obtenerUptimeTotal()) {
    tv = obtenerUptimeTotal();
  }
  unsigned long elapsed = obtenerUptimeTotal() - tv;
  if (elapsed >= perfilActivo.intervalo_volteo_ms) return 0;
  return (perfilActivo.intervalo_volteo_ms - elapsed) / 1000UL;
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
  Serial.print(F("Perfil: "));
  Serial.println(perfilActivo.nombre);
  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(perfilActivo.dias_total);
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
  Serial.print(motorVolteando ? "ON" : "OFF");
  Serial.print(F("  Fan: "));
  Serial.println(estadoVentilador ? "ON" : "OFF");
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
      "Perfil: %s\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Ventilador: %s\n"
      "Prox. volteo: %luh %lum\n"
      "Uptime: %luh",
      perfilActivo.nombre,
      diaActual, perfilActivo.dias_total,
      enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      temperatura, tempObjetivo,
      humedad, obtenerHumMin(), obtenerHumMax(),
      estadoCalefactor ? "ON" : "OFF",
      estadoHumificador ? "ON" : "OFF",
      motorVolteando ? "GIRANDO" : "OFF",
      estadoVentilador ? "ON" : "OFF",
      rest / 3600, (rest % 3600) / 60,
      uptimeH);
  } else {
    snprintf(buf, sizeof(buf),
      "INCUBADORA AUTOMATICA\n"
      "Perfil: %s\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Ventilador: %s\n"
      "Uptime: %luh",
      perfilActivo.nombre,
      diaActual, perfilActivo.dias_total,
      enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      temperatura, tempObjetivo,
      humedad, obtenerHumMin(), obtenerHumMax(),
      estadoCalefactor ? "ON" : "OFF",
      estadoHumificador ? "ON" : "OFF",
      motorVolteando ? "GIRANDO" : "OFF",
      estadoVentilador ? "ON" : "OFF",
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
  char json[768];
  snprintf(json, sizeof(json),
    "{"
    "\"temp\":%.1f,"
    "\"hum\":%.1f,"
    "\"temp_obj\":%.1f,"
    "\"temp_min\":%.1f,"
    "\"temp_max\":%.1f,"
    "\"hum_min\":%.1f,"
    "\"hum_max\":%.1f,"
    "\"cal\":%s,"
    "\"humidor\":%s,"
    "\"motor\":%s,"
    "\"fan\":%s,"
    "\"dia\":%d,"
    "\"dias_total\":%d,"
    "\"dia_lockdown\":%d,"
    "\"fase\":\"%s\","
    "\"lockdown\":%s,"
    "\"volteo_restante\":%lu,"
    "\"perfil\":\"%s\","
    "\"perfil_id\":%d,"
    "\"wifi\":%s,"
    "\"ip\":\"%s\","
    "\"bot\":%s,"
    "\"modo_ap\":%s,"
    "\"uptime\":%lu"
    "}",
    temperatura, humedad, tempObjetivo, tempMin, tempMax,
    obtenerHumMin(), obtenerHumMax(),
    estadoCalefactor ? "true" : "false",
    estadoHumificador ? "true" : "false",
    motorVolteando ? "true" : "false",
    estadoVentilador ? "true" : "false",
    diaActual, perfilActivo.dias_total, perfilActivo.dia_lockdown,
    enLockdown ? "LOCKDOWN" : "DESARROLLO",
    enLockdown ? "true" : "false",
    calcularTiempoVolteo(),
    perfilActivo.nombre,
    perfilIdActivo,
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : (modoAP ? "192.168.4.1" : ""),
    botListo ? "true" : "false",
    modoAP ? "true" : "false",
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
  } else if (action == "fan") {
    webCmdFan = true;
  } else if (action == "temp_up") {
    webCmdTempUp = true;
  } else if (action == "temp_down") {
    webCmdTempDown = true;
  } else if (action == "temp_set") {
    webCmdTempSet = value.toFloat();
  } else if (action == "profile") {
    webCmdProfileId = value.toInt();
  } else if (action == "custom") {
    webCmdCustom = true;
    webCmdCustomParams[0] = server.arg("dias_total").toFloat();
    webCmdCustomParams[1] = server.arg("dia_lockdown").toFloat();
    webCmdCustomParams[2] = server.arg("temp_obj").toFloat();
    webCmdCustomParams[3] = server.arg("temp_min").toFloat();
    webCmdCustomParams[4] = server.arg("temp_max").toFloat();
    webCmdCustomParams[5] = server.arg("hum_min_dev").toFloat();
    webCmdCustomParams[6] = server.arg("hum_max_dev").toFloat();
    webCmdCustomParams[7] = server.arg("hum_min_lock").toFloat();
    webCmdCustomParams[8] = server.arg("hum_max_lock").toFloat();
    webCmdCustomParams[9] = server.arg("intervalo_volteo_h").toFloat();
  } else if (action == "save") {
    webCmdSave = true;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleProfiles() {
  String json = "[";
  for (int i = 0; i < NUM_PERFILES; i++) {
    if (i > 0) json += ",";
    json += "{\"id\":" + String(i) + ",\"nombre\":\"" + String(PERFILES[i].nombre) + "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleHistory() {
  String json = "[";
  size_t count = historicoCount;
  if (count > MAX_HISTORICO) count = MAX_HISTORICO;
  json.reserve(count * 45 + 10);
  for (size_t i = 0; i < count; i++) {
    size_t idx = (historicoIndex + MAX_HISTORICO - count + i) % MAX_HISTORICO;
    if (i > 0) json += ",";
    json += "{\"t\":" + String(historico[idx].uptime) +
            ",\"temp\":" + String(historico[idx].temp, 1) +
            ",\"hum\":" + String(historico[idx].hum, 1) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleConfig() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String token = server.arg("token");
  String chats = server.arg("chats");

  if (ssid.length() > 0) {
    wifiSsid = ssid;
    wifiPass = pass;
  }
  if (token.length() > 0) {
    telegramToken = token;
    telegramToken.trim();
  }
  if (chats.length() > 0) {
    chatsPermitidos = chats;
  }

  guardarRed();

  if (ssid.length() > 0) {
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"WiFi guardado. Reiniciando...\"}");
    delay(500);
    ESP.restart();
  } else {
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Configuracion guardada.\"}");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/profiles", HTTP_GET, handleProfiles);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.onNotFound(handleRoot); // Captive portal: cualquier ruta sirve el dashboard
  server.begin();
  Serial.println(F("Servidor web iniciado en puerto 80."));
}

void aplicarPerfil(int id) {
  if (id < 0 || id >= NUM_PERFILES) return;
  perfilIdActivo = id;
  perfilActivo = PERFILES[id];
  tempObjetivo = perfilActivo.temp_objetivo;
  tempMin = perfilActivo.temp_min;
  tempMax = perfilActivo.temp_max;
  Serial.print(F("Perfil activo: "));
  Serial.println(perfilActivo.nombre);
  guardarEstado();
}

void aplicarCustomParams() {
  perfilIdActivo = NUM_PERFILES - 1; // Personalizado
  perfilActivo = PERFILES[perfilIdActivo];
  perfilActivo.dias_total = (int)webCmdCustomParams[0];
  perfilActivo.dia_lockdown = (int)webCmdCustomParams[1];
  perfilActivo.temp_objetivo = webCmdCustomParams[2];
  perfilActivo.temp_min = webCmdCustomParams[3];
  perfilActivo.temp_max = webCmdCustomParams[4];
  perfilActivo.hum_min_desarrollo = webCmdCustomParams[5];
  perfilActivo.hum_max_desarrollo = webCmdCustomParams[6];
  perfilActivo.hum_min_lockdown = webCmdCustomParams[7];
  perfilActivo.hum_max_lockdown = webCmdCustomParams[8];
  float horas = webCmdCustomParams[9];
  if (horas > 0) {
    perfilActivo.intervalo_volteo_ms = (unsigned long)(horas * 3600UL * 1000UL);
  }
  tempObjetivo = perfilActivo.temp_objetivo;
  tempMin = perfilActivo.temp_min;
  tempMax = perfilActivo.temp_max;
  Serial.println(F("Perfil personalizado aplicado."));
  guardarEstado();
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
  if (webCmdFan) {
    toggleVentilador();
    webCmdFan = false;
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
  if (webCmdProfileId >= 0) {
    aplicarPerfil(webCmdProfileId);
    webCmdProfileId = -1;
  }
  if (webCmdCustom) {
    aplicarCustomParams();
    webCmdCustom = false;
    for (int i = 0; i < 10; i++) webCmdCustomParams[i] = 0;
  }
  if (webCmdSave) {
    guardarEstado();
    webCmdSave = false;
  }
}

// ===================== BOT TELEGRAM =====================

void notificarTodos(const String& msg) {
  String resto = chatsPermitidos;
  while (resto.length() > 0) {
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
    String cmds = "[{\"command\":\"start\",\"description\":\"Bienvenida\"},{\"command\":\"help\",\"description\":\"Ayuda\"},{\"command\":\"info\",\"description\":\"Estado completo\"},{\"command\":\"temp\",\"description\":\"Fijar temp ej: temp 38\"},{\"command\":\"hum\",\"description\":\"Toggle humificador\"},{\"command\":\"cal\",\"description\":\"Toggle calefactor\"},{\"command\":\"volteo\",\"description\":\"Volteo forzado\"},{\"command\":\"guardar\",\"description\":\"Guardar estado\"},{\"command\":\"ota\",\"description\":\"Actualizar firmware (URL del bin)\"},{\"command\":\"reset\",\"description\":\"Reset estado\"}]";
    if (bot.setMyCommands(cmds)) {
      Serial.println(F("Comandos del bot registrados."));
    }
    comandosRegistrados = true;
    yield();
  }

  // Solo un lote de updates por ciclo para no bloquear el loop
  int num = bot.getUpdates(bot.last_message_received + 1);
  if (num > 0) {
    manejarMensajes(num);
    yield();
  }
}

void verificarAlertas() {
  if (!botListo) return;
  if (temperatura <= 0.0) return;

  bool fuera = (temperatura < tempMin || temperatura > tempMax);
  if (fuera && !alarmaTemp) {
    alarmaTemp = true;
    notificarTodos("ALERTA: temperatura fuera de rango: " + String(temperatura, 1) + "C");
  } else if (!fuera && alarmaTemp) {
    alarmaTemp = false;
    notificarTodos("OK: temperatura de nuevo en rango: " + String(temperatura, 1) + "C");
  }
}

// ===================== TAREA TELEGRAM (CORE 0) =====================

void actualizarSnapshot() {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap.temperatura = temperatura;
    snap.humedad = humedad;
    snap.tempObjetivo = tempObjetivo;
    snap.tempMin = tempMin;
    snap.tempMax = tempMax;
    snap.humMin = obtenerHumMin();
    snap.humMax = obtenerHumMax();
    snap.estadoCalefactor = estadoCalefactor;
    snap.estadoHumificador = estadoHumificador;
    snap.motorVolteando = motorVolteando;
    snap.estadoVentilador = estadoVentilador;
    snap.enLockdown = enLockdown;
    snap.diaActual = diaActual;
    snap.perfilDiasTotal = perfilActivo.dias_total;
    snap.perfilIdActivo = perfilIdActivo;
    snap.perfilNombre = perfilActivo.nombre;
    snap.uptime = obtenerUptimeTotal();
    snap.tiempoVolteoRestante = calcularTiempoVolteo();
    xSemaphoreGive(stateMutex);
  }
}

void enviarComando(TelegramCmdType cmd, float value = 0) {
  TelegramCmdMsg msg = { cmd, value };
  xQueueSend(telegramCmdQueue, &msg, pdMS_TO_TICKS(100));
}

String obtenerInfoSnapshot() {
  char buf[512];
  StateSnapshot s;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s = snap;
    xSemaphoreGive(stateMutex);
  } else {
    return "Error: no se pudo leer estado.";
  }
  unsigned long uptimeH = s.uptime / 3600000UL;
  if (!s.enLockdown) {
    unsigned long rest = s.tiempoVolteoRestante;
    snprintf(buf, sizeof(buf),
      "INCUBADORA AUTOMATICA\n"
      "Perfil: %s\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Ventilador: %s\n"
      "Prox. volteo: %luh %lum\n"
      "Uptime: %luh",
      s.perfilNombre,
      s.diaActual, s.perfilDiasTotal,
      s.enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      s.temperatura, s.tempObjetivo,
      s.humedad, s.humMin, s.humMax,
      s.estadoCalefactor ? "ON" : "OFF",
      s.estadoHumificador ? "ON" : "OFF",
      s.motorVolteando ? "GIRANDO" : "OFF",
      s.estadoVentilador ? "ON" : "OFF",
      rest / 3600, (rest % 3600) / 60,
      uptimeH);
  } else {
    snprintf(buf, sizeof(buf),
      "INCUBADORA AUTOMATICA\n"
      "Perfil: %s\n"
      "Dia: %d/%d\n"
      "Fase: %s\n"
      "Temp: %.1fC (obj %.1fC)\n"
      "Hum: %.1f%% (obj %.0f-%.0f%%)\n"
      "Calefactor: %s\n"
      "Humificador: %s\n"
      "Motor: %s\n"
      "Ventilador: %s\n"
      "Uptime: %luh",
      s.perfilNombre,
      s.diaActual, s.perfilDiasTotal,
      s.enLockdown ? "LOCKDOWN (eclosion)" : "DESARROLLO",
      s.temperatura, s.tempObjetivo,
      s.humedad, s.humMin, s.humMax,
      s.estadoCalefactor ? "ON" : "OFF",
      s.estadoHumificador ? "ON" : "OFF",
      s.motorVolteando ? "GIRANDO" : "OFF",
      s.estadoVentilador ? "ON" : "OFF",
      uptimeH);
  }
  return String(buf);
}

void manejarMensajesTelegram(int num) {
  StateSnapshot s;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s = snap;
    xSemaphoreGive(stateMutex);
  }

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
      if (chatsPermitidos.length() == 0) {
        agregarChat(chat);
        chatsPermitidos = chat;
      }
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
      bot.sendMessage(chat, obtenerInfoSnapshot(), "");
    } else if (cmd == "+" || cmd == "subir") {
      enviarComando(TCMD_SUBIR_TEMP);
      bot.sendMessage(chat, "Temp objetivo subiendo...", "");
    } else if (cmd == "-" || cmd == "bajar") {
      enviarComando(TCMD_BAJAR_TEMP);
      bot.sendMessage(chat, "Temp objetivo bajando...", "");
    } else if (cmd == "temp") {
      float v = arg.toFloat();
      if (v > 0) {
        enviarComando(TCMD_SET_TEMP, v);
        bot.sendMessage(chat, "Temp objetivo: " + String(v, 1) + "C", "");
      } else {
        bot.sendMessage(chat, "Uso: temp 38.0", "");
      }
    } else if (cmd == "h" || cmd == "hum" || cmd == "humificador") {
      enviarComando(TCMD_TOGGLE_HUM);
      bot.sendMessage(chat, "Humificador toggled.", "");
    } else if (cmd == "c" || cmd == "cal" || cmd == "calefactor") {
      enviarComando(TCMD_TOGGLE_CAL);
      bot.sendMessage(chat, "Calefactor toggled.", "");
    } else if (cmd == "t" || cmd == "vol" || cmd == "volteo") {
      if (s.enLockdown) {
        bot.sendMessage(chat, "Volteo bloqueado en LOCKDOWN.", "");
      } else {
        enviarComando(TCMD_FORZAR_VOLTEO);
        bot.sendMessage(chat, "Volteo iniciado.", "");
      }
    } else if (cmd == "s" || cmd == "save" || cmd == "guardar") {
      enviarComando(TCMD_SAVE);
      bot.sendMessage(chat, "Estado guardado.", "");
    } else if (cmd == "reset" || cmd == "r") {
      String argLow = arg;
      argLow.toLowerCase();
      if (argLow == "si" || argLow == "yes") {
        enviarComando(TCMD_RESET);
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

void verificarAlertasTelegram() {
  if (temperatura <= 0.0) return;

  bool fuera = (temperatura < tempMin || temperatura > tempMax);
  if (fuera && !alarmaTemp) {
    alarmaTemp = true;
    notificarTodos("ALERTA: temperatura fuera de rango: " + String(temperatura, 1) + "C");
  } else if (!fuera && alarmaTemp) {
    alarmaTemp = false;
    notificarTodos("OK: temperatura de nuevo en rango: " + String(temperatura, 1) + "C");
  }
}

void telegramTask(void* parameter) {
  Serial.println(F("Tarea Telegram iniciada en core 0."));

  bool comandosReg = false;
  unsigned long ultimoCheck = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));

    // Actualizar snapshot periodicamente (cada 2s)
    actualizarSnapshot();

    // Verificar si Telegram esta listo
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    static bool ntpLocal = false;
    if (wifiOk && !ntpLocal) {
      time_t now = time(nullptr);
      if (now > 24 * 3600) ntpLocal = true;
    }
    bool listo = wifiOk && ntpLocal && telegramToken.length() > 0;

    if (!listo) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    // Registrar comandos del bot una vez
    if (!comandosReg) {
      String cmds = "[{\"command\":\"start\",\"description\":\"Bienvenida\"},{\"command\":\"help\",\"description\":\"Ayuda\"},{\"command\":\"info\",\"description\":\"Estado completo\"},{\"command\":\"temp\",\"description\":\"Fijar temp ej: temp 38\"},{\"command\":\"hum\",\"description\":\"Toggle humificador\"},{\"command\":\"cal\",\"description\":\"Toggle calefactor\"},{\"command\":\"volteo\",\"description\":\"Volteo forzado\"},{\"command\":\"guardar\",\"description\":\"Guardar estado\"},{\"command\":\"ota\",\"description\":\"Actualizar firmware (URL del bin)\"},{\"command\":\"reset\",\"description\":\"Reset estado\"}]";
      if (bot.setMyCommands(cmds)) {
        Serial.println(F("Comandos del bot registrados."));
      }
      comandosReg = true;
    }

    // Obtener actualizaciones de Telegram (la llamada bloqueante ocurre aqui, en core 0)
    int num = bot.getUpdates(bot.last_message_received + 1);
    if (num > 0) {
      manejarMensajesTelegram(num);
    }

    // Verificar alertas de temperatura
    verificarAlertasTelegram();

    // Verificar si hay notificacion de lockdown pendiente
    if (notifyLockdown) {
      notifyLockdown = false;
      notificarTodos("LOCKDOWN - Dia 18+. Volteo detenido. Humedad objetivo 65-70%.");
    }

    // Esperar intervalo antes del proximo ciclo
    vTaskDelay(pdMS_TO_TICKS(INTERVALO_BOT));
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
  if (!displayOk) return;
  gfx->fillScreen(COLOR_BLACK);

  // ---- TITULO ----
  gfx->setCursor(15, 0);
  gfx->setTextColor(COLOR_YELLOW);
  gfx->setTextSize(3);
  gfx->println("INCUBADORA");

  // Perfil activo
  gfx->setTextSize(1);
  gfx->setCursor(0, 24);
  gfx->setTextColor(COLOR_CYAN);
  gfx->print(perfilActivo.nombre);

  // ---- TEMPERATURA ----
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
  gfx->println(perfilActivo.dias_total);

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
  int barraProgreso = (int)((float)diaActual / perfilActivo.dias_total * barraAncho);
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
  gfx->setCursor(160, 296);
  if (estadoVentilador) {
    gfx->setTextColor(COLOR_GREEN);
    gfx->print("F");
  } else {
    gfx->setTextColor(COLOR_RED);
    gfx->print("f");
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

  Serial.println(F("\n=== Incubadora Automatica v2.0 ==="));

  // Proteccion contra crash-loop por NVS corrupto tras cortes de energia
  {
    Preferences bootCtrl;
    bootCtrl.begin("boot_ctrl", false);
    int bootCount = bootCtrl.getInt("count", 0) + 1;
    bootCtrl.putInt("count", bootCount);
    bootCtrl.end();

    if (bootCount > 3) {
      Serial.println(F("CRASH-LOOP detectado (>3 reinicios). Borrando NVS..."));
      Preferences wipe;
      wipe.begin("incubadora", false);
      wipe.clear();
      wipe.end();
      bootCtrl.begin("boot_ctrl", false);
      bootCtrl.putInt("count", 0);
      bootCtrl.end();
      Serial.println(F("NVS limpiado. Arrancando con valores por defecto."));
    }
  }

  // Relays OFF cuanto antes (HIGH antes de OUTPUT evita pico de corriente al boot)
  relaysInitSeguro();
  millisArranque = millis();

  // Watchdog: Arduino ya inicializa el TWDT. Reconfigurar para cubrir ambos cores.
  esp_task_wdt_config_t twdt_config = {};
  twdt_config.timeout_ms = 30000;
  twdt_config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
  twdt_config.trigger_panic = true;
  esp_task_wdt_reconfigure(&twdt_config);
  Serial.println(F("Watchdog reconfigurado (30s, ambos cores)."));
  Serial.println(F("Relays en OFF (arranque seguro)."));

  // Pantalla con retry (max 2 intentos, no bloquea el sistema si falla)
  for (int intento = 0; intento < 2; intento++) {
    esp_task_wdt_reset();
    if (gfx->begin()) {
      displayOk = true;
      break;
    }
    Serial.printf("Intento display %d fallido, reintentando...\n", intento + 1);
    delay(500);
  }
  if (displayOk) {
    Serial.println(F("Pantalla TFT OK"));
  } else {
    Serial.println(F("ERROR: Pantalla TFT no detectada tras 2 intentos. Continuando sin display."));
  }

  // Splash screen (solo si display disponible)
  if (displayOk) {
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
    delay(200);
  }

  // Cargar estado persistente y config de red (antes del sensor para usar settings)
  cargarEstado();
  cargarRed();

  // Calcular dia con uptime acumulado
  calcularDia();
  verificarFase();

  // Sensor DHT22: init + warmup no-bloqueante
  am2302.begin();
  am2302.read();
  esp_task_wdt_reset();
  sensorWarmupCount = 1;
  sensorCalentando = true;

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
    inicioIntentoWiFi = millis();
    intentoWiFiInicial = true;
  } else {
    Serial.println(F("Sin credenciales WiFi. Modo AP activo."));
    iniciarModoAP();
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
    digitalWrite(PIN_RELAY_FAN, HIGH);
    Serial.println(F("OTA iniciada..."));
  });
  ArduinoOTA.begin();
  Serial.println(F("OTA listo (ArduinoOTA)."));

  // Web Server Dashboard
  setupWebServer();

  // Crear mutex y cola de comandos para la tarea de Telegram
  stateMutex = xSemaphoreCreateMutex();
  telegramCmdQueue = xQueueCreate(16, sizeof(TelegramCmdMsg));

  if (stateMutex && telegramCmdQueue) {
    Serial.println(F("Sincronizacion inter-tareas OK."));
  } else {
    Serial.println(F("ERROR: No se pudo crear mutex/cola."));
  }

  // Copiar estado inicial al snapshot
  actualizarSnapshot();

  // Iniciar tarea de Telegram en core 0 (4096 bytes de stack)
  BaseType_t result = xTaskCreatePinnedToCore(
    telegramTask,       // funcion de la tarea
    "TelegramTask",     // nombre
    8192,               // stack size (Telegram HTTPS necesita espacio)
    NULL,               // parametro
    2,                  // prioridad (2 = superior a loop default 1)
    &telegramTaskHandle,// handle
    0                   // core 0
  );

  if (result == pdPASS) {
    Serial.println(F("Tarea Telegram creada en core 0."));
  } else {
    Serial.println(F("ERROR: No se pudo crear tarea Telegram."));
  }

  Serial.print(F("Dia: "));
  Serial.print(diaActual);
  Serial.print(F("/"));
  Serial.println(perfilActivo.dias_total);
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

  // Limpiar contador de crash-loop tras 10s de uptime estable
  static bool bootCtrlCleared = false;
  if (!bootCtrlCleared && uptimeAcumulado > 10000UL) {
    Preferences bootCtrl;
    bootCtrl.begin("boot_ctrl", false);
    bootCtrl.putInt("count", 0);
    bootCtrl.end();
    bootCtrlCleared = true;
  }

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

  // Completar warmup del sensor DHT22 en background (1 lectura por ciclo)
  if (sensorCalentando && sensorWarmupCount < 5) {
    am2302.read();
    sensorWarmupCount++;
    if (sensorWarmupCount >= 5) {
      sensorCalentando = false;
      Serial.println(F("Sensor DHT22 listo."));
    }
  }

  // Leer sensor y calcular fase
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    leerSensor();
    calcularDia();
    verificarFase();
    guardarMuestraHistorico(ahora);
    ultimaLectura = ahora;
  }

  // Actuadores solo tras estabilizar rail 5V (evita brown-out con puente JD-VCC)
  if (!actuadoresListos && (ahora - millisArranque >= DELAY_ACTUADORES_MS)) {
    actuadoresListos = true;
    Serial.println(F("Actuadores habilitados."));
  }

  // Controlar actuadores
  controlarCalefactor();
  controlarHumificador();
  controlarVentilador();
  verificarVolteo(ahora);

  // Seguridad motor: forzar apagado si lleva mas de 30s encendido
  if (motorVolteando && (ahora - inicioVolteo > 30000UL)) {
    Serial.println(F("ALERTA: Motor timeout 30s. Apagando forzado."));
    detenerVolteo();
  }

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

  // Telegram ahora corre en tarea separada (core 0).
  // Aqui solo procesamos comandos recibidos desde el bot.
  TelegramCmdMsg tCmd;
  while (xQueueReceive(telegramCmdQueue, &tCmd, 0) == pdTRUE) {
    switch (tCmd.cmd) {
      case TCMD_SUBIR_TEMP:   subirTemp(); break;
      case TCMD_BAJAR_TEMP:   bajarTemp(); break;
      case TCMD_SET_TEMP:     tempObjetivo = tCmd.value; break;
      case TCMD_TOGGLE_HUM:   toggleHumManual(); break;
      case TCMD_TOGGLE_CAL:   toggleCalManual(); break;
      case TCMD_FORZAR_VOLTEO: forzarVolteoManual(); break;
      case TCMD_SAVE:         guardarEstado(); break;
      case TCMD_RESET:        borrarEstado(); break;
      case TCMD_TOGGLE_VENTILADOR: toggleVentilador(); break;
      default: break;
    }
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
