# Incubadora Automatica v2.0 - ESP32

## Mapeo de Conexiones

### Pantalla TFT ST7789 (170x320 IPS)

| Pin Pantalla | Pin ESP32 | Función       |
|-------------|-----------|---------------|
| BLK         | 3.3V      | Backlight     |
| DC          | GPIO 16 (RX2) | Data/Command  |
| CS          | GPIO 5        | Chip Select   |
| RES         | GPIO 17 (TX2) | Reset         |
| SDA         | GPIO 23       | Data SPI (MOSI) |
| SCL         | GPIO 18       | Clock SPI (SCK) |
| VCC         | 3.3V      | Alimentación  |
| GND         | GND       | Tierra        |

> MISO no se conecta. Resolución: 170x320. Tipo: IPS.
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

### Módulo Relay 3 Canales

| Canal Relay | Pin ESP32 | Control       |
|-------------|-----------|---------------|
| Canal 1     | GPIO 25   | Calefactor    |
| Canal 2     | GPIO 26   | Humificador   |
| Canal 3     | GPIO 27   | Motor Rodillos |

> Relays activos en LOW (Relay IN a GPIO, VCC a 3.3V/5V según módulo).

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
```

## Librerías Requeridas
- AM2302-Sensor
- Arduino_GFX_Library
- UniversalTelegramBot
- ArduinoJson (dependencia del bot)

## WiFi y Bot de Telegram

El ESP32 guarda las credenciales WiFi, el token del bot y los chats permitidos en la flash (Preferences), así no hay que recompilar para cambiarlas.

### Configuración por serial (115200 baud)

1. `wifi MiRed miClave` → guarda y conecta al WiFi.
2. `token 123456:ABC...` → guarda el token del bot (obtenlo con @BotFather).
3. Abre el chat con tu bot y envía `/start` (el primer chat que lo haga queda autorizado automáticamente). También puedes agregar chats con `allow <chat_id>`.

### Comandos del bot de Telegram

| Comando    | Acción                       |
|------------|------------------------------|
| info       | Estado completo (temp, hum, día, relays, volteo, uptime) |
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

> El bot también avisa automáticamente si la temperatura sale del rango (37.2–37.8 °C) y al entrar en LOCKDOWN (día 18+).

## Actualización OTA (sin cable)

El proyecto usa una partición personalizada (`incubadora.csv`) con 2 slots de app (~1,69MB cada uno), así el OTA cabe en flash de 4MB.

### Configuración una sola vez
1. En Arduino IDE: **Tools → Partition Scheme → Custom** (usa `incubadora.csv` del sketch).
2. Sube por USB una vez para grabar la tabla de particiones.
3. Después del arranque, en **Tools → Port** aparece `incubadora` como puerto de red (ArduinoOTA).

### Actualizar desde el IDE (por WiFi)
- Selecciona el puerto de red `incubadora` en *Tools → Port* y pulsa Upload (sin cable USB).

### Actualizar por Telegram (`/ota`)
1. Compila y exporta el binario: *Sketch → Export Compiled Binary*.
2. Sube `incubadora.ino.bin` a un **GitHub Release** y copia la URL de descarga directa:
   `https://github.com/<user>/<repo>/releases/download/<tag>/incubadora.ino.bin`
3. Al bot: `ota <esa_url>` (solo chats autorizados). El ESP32 descarga, instala y reinicia. Si la escritura falla, sigue con la versión actual; si el nuevo firmware no arranca, revierte automáticamente.

> También por serial: `ota <url>`.

## Comandos Serial (115200 baud)

| Comando          | Acción                             |
|------------------|------------------------------------|
| +/-              | Ajustar temp objetivo              |
| temp 38.0        | Fijar temp objetivo                |
| h                | Toggle humificador manual          |
| c                | Toggle calefactor manual           |
| t                | Volteo forzado                     |
| s                | Guardar estado en flash            |
| d / info         | Mostrar info                       |
| r                | Reset (borrar estado de incubación)|
| wifi             | Estado de la red                   |
| wifi ssid pass   | Guardar credenciales y conectar (también `wifi s=ssid p=pass`) |
| token <TOKEN>    | Guardar token de Telegram (también `token t=<TOKEN>`; valida formato) |
| delwifi / deltoken | Borrar credenciales/token        |
| allow <id>       | Permitir chat de Telegram          |
| block <id>       | Bloquear chat de Telegram          |
| ota <url>        | Actualizar firmware (OTA)          |
| help             | Ayuda                              |
