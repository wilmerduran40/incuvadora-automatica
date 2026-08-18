# Incubadora Automatica - Version Minima Estable

Firmware simplificado para ESP32 destinado al control de una incubadora automatica.

Esta version elimina componentes que consumian muchos recursos y podian causar bloqueos:
- Dashboard web, servidor DNS y modo AP.
- Actualizacion OTA (ArduinoOTA y OTA por URL).
- Tarea FreeRTOS dedicada a Telegram.
- Perfiles predefinidos.
- Comandos de Telegram que controlan actuadores.

Telegram ahora se usa **unicamente para monitoreo remoto** y alertas de temperatura fuera de rango.

## Componentes

- ESP32
- Sensor DHT22 (AM2302)
- Modulo relay 3 canales (calefactor, humificador, motor de volteo)
- Pantalla TFT ST7789 170x320 IPS
- Fuente 5V/2A minimo para el ESP32, TFT y logica del relay
- Fuente adicional recomendada para las bobinas del relay (JD-VCC)

## Conexiones

### Modulo relay 3 canales

| Modulo relay | Conexion                                              |
|--------------|-------------------------------------------------------|
| JD-VCC       | 5V de fuente dedicada a bobinas (recomendado)         |
| VCC          | 3.3V del ESP32 (logica optoacoplador)                 |
| GND          | GND comun entre modulo relay y ESP32                  |
| IN1          | GPIO 25 - Calefactor                                  |
| IN2          | GPIO 26 - Humificador                                 |
| IN3          | GPIO 27 - Motor de volteo                             |

**Importante:** quitar el puente amarillo JD-VCC/VCC del modulo relay para aislar las bobinas del 3.3V del ESP32.

### Pantalla TFT ST7789

| Pin pantalla | Pin ESP32 | Funcion       |
|--------------|-----------|---------------|
| BLK          | 3.3V      | Backlight     |
| DC           | GPIO 16   | Data/Command  |
| CS           | GPIO 5    | Chip Select   |
| RES          | GPIO 17   | Reset         |
| SDA          | GPIO 23   | MOSI          |
| SCL          | GPIO 18   | SCK           |
| VCC          | 3.3V      | Alimentacion  |
| GND          | GND       | Tierra        |

### Sensor DHT22

| Pin sensor | Pin ESP32 |
|------------|-----------|
| DATA       | GPIO 4    |
| VCC        | 3.3V      |
| GND        | GND       |

### Resumen de pines ESP32

```
GPIO 4   - DHT22 DATA
GPIO 5   - TFT CS
GPIO 16  - TFT DC
GPIO 17  - TFT RES
GPIO 18  - TFT SCL
GPIO 23  - TFT SDA
GPIO 25  - Relay calefactor
GPIO 26  - Relay humificador
GPIO 27  - Relay motor volteo
```

## Proteccion electrica recomendada

- **Capacitor electrolitico 1000uF/35V** entre 5V y GND lo mas cerca posible del ESP32.
- **Capacitor ceramico 100nF** en paralelo para filtrar alta frecuencia.
- **Puente JD-VCC quitado** y fuente separada para el relay.
- Los actuadores de 110V (calefactor, humificador, motor) deben conectarse solo a traves del lado del relay, sin compartir GND con el ESP32.

## Librerias requeridas

- AM2302-Sensor
- Arduino_GFX_Library
- UniversalTelegramBot
- ArduinoJson (dependencia de UniversalTelegramBot)

## Configuracion inicial

### 1. Cargar el firmware

Abrir `incubadora/incubadora.ino` en Arduino IDE, seleccionar la placa ESP32 y subir.

### 2. Configurar WiFi y Telegram por serial

Conectar a 115200 baud y enviar:

```
wifi TU_SSID TU_CLAVE
token 123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ
```

El token se obtiene con [@BotFather](https://t.me/BotFather) en Telegram.

### 3. Autorizar el chat de Telegram

Enviar `/start` al bot. El primer chat que lo haga queda autorizado automaticamente. Tambien se puede autorizar manualmente por serial:

```
allow 123456789
```

## Comandos seriales

### Control rapido

| Comando | Accion                                  |
|---------|-----------------------------------------|
| `+` / `-` | Subir/bajar temp objetivo 0.5C        |
| `temp 38.0` | Fijar temperatura objetivo            |
| `h`       | Toggle humificador manual               |
| `c`       | Toggle calefactor manual                |
| `t`       | Volteo forzado                          |
| `s`       | Guardar estado en NVS                   |
| `r`       | Borrar estado y reiniciar               |
| `d` / `info` | Mostrar informacion completa         |
| `params`  | Mostrar parametros personalizados       |
| `help`    | Mostrar ayuda                           |

### Configurar parametros del perfil

| Comando           | Descripcion                          |
|-------------------|--------------------------------------|
| `set dias 21`     | Dias totales de incubacion           |
| `set lock 18`     | Dia de inicio del lockdown           |
| `set tobj 37.6`   | Temperatura objetivo                 |
| `set tmin 37.5`   | Temperatura minima (calefactor ON)   |
| `set tmax 37.8`   | Temperatura maxima (calefactor OFF)  |
| `set hmin 50`     | Humedad minima en desarrollo         |
| `set hmax 55`     | Humedad maxima en desarrollo         |
| `set hlmin 65`    | Humedad minima en lockdown           |
| `set hlmax 70`    | Humedad maxima en lockdown           |
| `set vol 2`       | Intervalo de volteo en horas         |
| `set dur 20`      | Duracion de volteo en segundos       |

**Importante:** despues de cambiar parametros, enviar `s` para guardar en NVS.

### Configuracion de red y Telegram

| Comando              | Accion                                 |
|----------------------|----------------------------------------|
| `wifi`               | Ver estado de WiFi                     |
| `wifi SSID CLAVE`    | Guardar y conectar WiFi                |
| `token TOKEN`        | Guardar token del bot                  |
| `allow ID`           | Permitir chat                          |
| `block ID`           | Bloquear chat                          |
| `delwifi`            | Borrar credenciales WiFi               |
| `deltoken`           | Borrar token del bot                   |

## Comandos de Telegram

| Comando                  | Accion                                      |
|--------------------------|---------------------------------------------|
| `/start`                 | Bienvenida y autorizacion del primer chat   |
| `info` / `estado`        | Estado completo de la incubadora            |
| `help`                   | Lista de comandos                           |
| `id`                     | Mostrar tu chat_id                          |

**No se permiten comandos de control** de actuadores, reset ni OTA por Telegram. Solo lectura y alertas.

## Alertas automaticas

Si la temperatura sale del rango configurado (`tmin` a `tmax`), el bot envia:

```
ALERTA: temperatura fuera de rango: XX.XC
```

Cuando vuelve al rango, envia:

```
OK: temperatura en rango: XX.XC
```

## Fases de incubacion

El sistema usa dos fases basadas en el dia actual:

- **Desarrollo** (dia 1 hasta `lock - 1`): volteo automatico activo, humedad segun `hmin`/`hmax`.
- **Lockdown** (dia `lock` en adelante): volteo desactivado, humedad segun `hlmin`/`hlmax`.

El dia se calcula a partir del uptime acumulado, que se guarda en NVS cada 60 segundos para proteger contra cortes de energia.

## Plan de prueba recomendado

1. **Subir el firmware** y verificar que la pantalla muestre la interfaz y el sensor lea temperatura/humedad.
2. **Prueba sin carga:** dejar encendido al menos 30 minutos sin conectar actuadores de 110V. Los relays pueden hacer click, pero el sistema no debe colgarse.
3. **Prueba con calefactor:** conectar solo el calefactor y observar al menos 20 minutos.
4. **Prueba completa:** conectar humificador y motor, observar comportamiento del volteo.
5. **Prueba de cortes:** desconectar la alimentacion unos segundos y verificar que al reconectar recupere el dia y parametros.

## Medidas de seguridad del firmware

- Relays inicializados en OFF antes de configurar los pines como salida.
- Actuadores habilitados solo despues de 2 segundos de arranque y con sensor valido.
- Cambios de relay escalonados 150 ms para no activar varias bobinas a la vez.
- Timeout de seguridad del motor: se apaga forzosamente a los 30 segundos.
- Fail-safe del sensor: si hay 5 lecturas consecutivas invalidas, se apagan calefactor y humificador.
- Watchdog de 5 segundos: reinicia el ESP32 si el loop principal se bloquea.
- Si no hay WiFi configurado, el sistema sigue funcionando localmente sin intentar abrir modo AP.

## Solucion de problemas

### La incubadora se bloquea o reinicia sola

1. Verificar que el puente JD-VCC del relay este quitado.
2. Verificar capacitor 1000uF en la entrada de 5V del ESP32.
3. Verificar que la fuente de 5V entregue al menos 2A estables.
4. Asegurar que los GND del relay y del ESP32 esten unidos, pero que los actuadores de 110V no compartan GND con el ESP32.
5. Revisar por serial si hay mensajes de error del sensor o del watchdog.

### Telegram no responde

1. Verificar que `token` este configurado correctamente.
2. Verificar que el chat este autorizado (`allow ID`).
3. Verificar que el ESP32 tenga conexion WiFi (comando `wifi` por serial).
4. Esperar hasta 30 segundos; Telegram se consulta cada ese intervalo.

### Los parametros se pierden al reiniciar

Enviar `s` por serial despues de cualquier cambio con `set`. El guardado automatico ocurre cada 60 segundos.

## Archivos

- `incubadora/incubadora.ino` - firmware simplificado actual.
- `incubadora/incubadora.ino.backup.original` - version completa anterior por si se necesita recuperar.
