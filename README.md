# Incubadora Automatica v2.0 - ESP32

## Mapeo de Conexiones

### Pantalla TFT ST7789 (170x320 IPS)

| Pin Pantalla | Pin ESP32 | Funcion       |
|-------------|-----------|---------------|
| BLK         | 3.3V      | Backlight     |
| DC          | GPIO 16 (RX2) | Data/Command  |
| CS          | GPIO 5        | Chip Select   |
| RES         | GPIO 17 (TX2) | Reset         |
| SDA         | GPIO 23       | Data SPI (MOSI) |
| SCL         | GPIO 18       | Clock SPI (SCK) |
| VCC         | 3.3V      | Alimentacion  |
| GND         | GND       | Tierra        |

> MISO no se conecta. Resolucion: 170x320. Tipo: IPS.
>
> `Arduino_ESP32SPI(dc=16, cs=5, sck=18, mosi=23)`
>
> `Arduino_ST7789(bus, rst=17, rotation=0, ips=true, w=170, h=320, col_offset1=35, row_offset1=0, col_offset2=35, row_offset2=0)`

### Sensor DHT22 (AM2302)

| Pin Sensor | Pin ESP32 |
|-----------|-----------|
| DATA      | GPIO 4    |
| VCC       | 3.3V      |
| GND       | GND       |

### Modulo Relay 4 Canales

| Canal Relay | Pin ESP32 | Control       |
|-------------|-----------|---------------|
| Canal 1     | GPIO 25   | Calefactor    |
| Canal 2     | GPIO 26   | Humificador   |
| Canal 3     | GPIO 27   | Motor Rodillos |
| Canal 4     | GPIO 33   | Ventilador    |

> Relays **activos en LOW** (IN a GPIO). Firmware: HIGH antes de `pinMode(OUTPUT)` y actuadores habilitados ~500 ms tras el boot para evitar brown-out.

#### Puente amarillo (JD-VCC) — importante

Muchos modulos 4 canales traen un **jumper amarillo** entre JD-VCC y VCC:

| Modo | Que hacer | Efecto |
|------|-----------|--------|
| **Recomendado** | **Quitar el puente** | Bobinas aisladas del 3.3V del ESP32 |
| Con puente | Comparten rail | Pico de corriente al reinicio puede congelar el ESP32 |

**Cableado recomendado (sin puente):**

```
Fuente 5V ──► JD-VCC (bobinas)     ESP32 3.3V ──► VCC (lado opto/logica)
Fuente GND ──► GND del modulo  ◄── GND ESP32 (GND comun obligatorio)
GPIO 25/26/27/33 ──► IN1..IN4
```

**Si dejas el puente puesto:** el firmware mitiga el arranque (relays OFF + delay), pero sigue siendo preferible capacitor 1000uF y fuente 5V con margen.

## Resumen de Pines ESP32

```
GPIO 4   — DHT22 (DATA)
GPIO 5   — TFT CS
GPIO 16  — TFT DC
GPIO 17  — TFT RES
GPIO 18  — TFT SCL
GPIO 23  — TFT SDA
GPIO 25  — Relay Calefactor
GPIO 26  — Relay Humificador
GPIO 27  — Relay Motor Rodillos
GPIO 33  — Relay Ventilador
```

## Proteccion Electrica

### 1. Arranque seguro de relays (firmware)

Al reiniciar, `pinMode(OUTPUT)` en ESP32 deja el pin en LOW un instante. Con relays active-LOW eso enciende varias bobinas a la vez y puede tumbar el 5V.

El firmware:
1. Escribe HIGH en los 4 IN **antes** de `pinMode(OUTPUT)`.
2. Mantiene todos los relays OFF los primeros ~500 ms.
3. Escaloná cambios de relay (~50 ms) para no activar varias bobinas a la vez.

### 2. Capacitor de desacoplo (1000uF / 35V)

El modulo relay y el ESP32 suelen compartir la entrada de 5V. Cuando las bobinas se activan, los picos de corriente pueden causar caidas de voltaje (brown-out) que congelan o resetean el ESP32.

**Solucion:** Capacitor electrolitico de 1000uF (35V) instalado en paralelo con la entrada de 5V, lo mas cerca posible de los pines del ESP32.

```
Fuente 5V ─────┬──── ESP32 (VIN/5V)
               │
               ├── (+) Capacitor 1000uF 35V
               │
GND ───────────┴──── ESP32 GND
```

> **IMPORTANTE:** Respetar polaridad. Linea blanca del capacitor = GND. Terminal largo = 5V.
>
> Opcional: agregar capacitor ceramico 100nF en paralelo para filtrado de alta frecuencia.

## Arquitectura de Tareas (FreeRTOS)

El ESP32 tiene 2 cores. El firmware los usa para separar el control de actuadores del trafico de red:

| Tarea | Core | Funcion |
|-------|------|---------|
| `loop()` (principal) | Core 1 | Sensores, actuadores, pantalla TFT, serial, web server |
| `telegramTask()` | Core 0 | Bot Telegram, WiFi, alertas, notificaciones |

### Proteccion contra congelamiento (3 capas)

| Capa | Mecanismo | Protege contra |
|------|-----------|----------------|
| 1. Capacitor 1000uF | Filtro electrico en rail 5V | Brown-out por picos de relay |
| 2. Watchdog timer (10s) | Resetea el ESP32 si el loop se bloquea | Cuelgue de software / HTTPS bloqueante |
| 3. Motor timeout (30s) | Apaga motor forzosamente si lleva 30s encendido | Motor 110V encendido indefinidamente |

### Comunicacion entre tareas

- **`StateSnapshot` + Mutex:** El loop copia el estado periodicamente a un snapshot protegido. Telegram lee la copia (sin acceder a variables compartidas).
- **`Queue<TelegramCmdMsg>`:** Telegram envia comandos (subir/bajar temp, toggle cal/hum, volteo, etc.) al loop principal via una cola FreeRTOS.

## Librerias Requeridas
- AM2302-Sensor
- Arduino_GFX_Library
- UniversalTelegramBot
- ArduinoJson (dependencia del bot)

## Perfiles de Incubacion

El sistema incluye perfiles predefinidos y personalizable:

| Perfil | Dias | Lockdown | Temp | Hum desarrollo | Hum lockdown | Intervalo volteo |
|--------|------|----------|------|----------------|--------------|------------------|
| Pollo | 21 | Dia 18 | 37.6C | 50-55% | 65-70% | 2h (20s) |
| Codorniz | 18 | Dia 14 | 37.5C | 45-50% | 60-65% | 2h (20s) |
| Pavo | 28 | Dia 25 | 37.5C | 50-55% | 65-70% | 2h (25s) |
| Pato | 28 | Dia 25 | 37.5C | 55-60% | 70-75% | 2h (25s) |
| Personalizado | - | - | - | - | - | - |

> **Desarrollo (dias 1-17):** Volteo automatico cada 2h, humedad 50-55%.
>
> **Lockdown (dia 18+):** Volteo detenido, humedad 65-70%.

## Fases de Incubacion

```
Dia 1 -------- Dia 17 -------- Dia 18 -------- Dia 21
|   DESARROLLO   |   LOCKDOWN (eclosion)   |
| Volteo ON 2h   | Volteo OFF              |
| Hum 50-55%     | Hum 65-70%              |
```

## Proteccion contra Cortes Electricos

El estado se guarda automaticamente cada 60 segundos en NVS flash (Preferences):
- Uptime acumulado
- Ultimo volteo
- Perfil activo y parametros
- Estado manual (calefactor/humificador)
- Estado ventilador

Al reconectar, el ESP32 recupera el dia de incubacion calculando desde el uptime total.

## WiFi y Bot de Telegram

El ESP32 guarda las credenciales WiFi, el token del bot y los chats permitidos en la flash (Preferences), asi no hay que recompilar para cambiarlas.

### Configuracion por serial (115200 baud)

1. `wifi MiRed miClave` -> guarda y conecta al WiFi.
2. `token 123456:ABC...` -> guarda el token del bot (obtenlo con @BotFather).
3. Abre el chat con tu bot y envia `/start` (el primer chat que lo haga queda autorizado automaticamente). Tambien puedes agregar chats con `allow <chat_id>`.

### Comandos del bot de Telegram

| Comando    | Accion                       |
|------------|------------------------------|
| info       | Estado completo (temp, hum, dia, relays, volteo, uptime) |
| + / -      | Subir/bajar temp objetivo    |
| temp 38.0  | Fijar temp objetivo          |
| h          | Toggle humificador manual    |
| c          | Toggle calefactor manual     |
| t          | Volteo forzado               |
| s          | Guardar estado en flash      |
| reset si   | Reset (borrar estado)        |
| wifi       | Estado de la red             |
| id         | Mostrar tu chat_id           |
| help       | Ayuda                        |

> El bot tambien avisa automaticamente si la temperatura sale del rango y al entrar en LOCKDOWN (dia 18+).

## Dashboard Web

El ESP32 ejecuta un servidor web en puerto 80 con dashboard completo:
- Monitoreo en tiempo real (temp, hum, relays, fase)
- Control remoto (calefactor, humificador, ventilador, volteo)
- Cambio de perfiles
- Grafico historico (24h)
- Configuracion WiFi/Telegram

**Acceso:** Conectate a la IP del ESP32 desde cualquier navegador en la misma red.

> Si no hay WiFi configurado, el ESP32 activa modo AP: `Incubadora-Setup` / `incubadora123`.

## Actualizacion OTA (sin cable)

El proyecto usa una particion personalizada (`incubadora.csv`) con 2 slots de app (~1,69MB cada uno), asi el OTA cabe en flash de 4MB.

### Configuracion una sola vez
1. En Arduino IDE: **Tools -> Partition Scheme -> Custom** (usa `incubadora.csv` del sketch).
2. Sube por USB una vez para grabar la tabla de particiones.
3. Despues del arranque, en **Tools -> Port** aparece `incubadora` como puerto de red (ArduinoOTA).

### Actualizar desde el IDE (por WiFi)
- Selecciona el puerto de red `incubadora` en *Tools -> Port* y pulsa Upload (sin cable USB).

### Actualizar por Telegram (`ota`)
1. Compila y exporta el binario: *Sketch -> Export Compiled Binary*.
2. Sube `incubadora.ino.bin` a un **GitHub Release** y copia la URL de descarga directa:
   `https://github.com/<user>/<repo>/releases/download/<tag>/incubadora.ino.bin`
3. Al bot: `ota <esa_url>` (solo chats autorizados). El ESP32 descarga, instala y reinicia. Si la escritura falla, sigue con la version actual; si el nuevo firmware no arranca, revierte automaticamente.

> Tambien por serial: `ota <url>`.

## Comandos Serial (115200 baud)

| Comando          | Accion                             |
|------------------|------------------------------------|
| +/-              | Ajustar temp objetivo              |
| temp 38.0        | Fijar temp objetivo                |
| h                | Toggle humificador manual          |
| c                | Toggle calefactor manual           |
| t                | Volteo forzado                     |
| s                | Guardar estado en flash            |
| d / info         | Mostrar info                       |
| r                | Reset (borrar estado de incubacion)|
| wifi             | Estado de la red                   |
| wifi ssid pass   | Guardar credenciales y conectar (tambien `wifi s=ssid p=pass`) |
| token <TOKEN>    | Guardar token de Telegram (tambien `token t=<TOKEN>`; valida formato) |
| delwifi / deltoken | Borrar credenciales/token        |
| allow <id>       | Permitir chat de Telegram          |
| block <id>       | Bloquear chat de Telegram          |
| ota <url>        | Actualizar firmware (OTA)          |
| help             | Ayuda                              |
