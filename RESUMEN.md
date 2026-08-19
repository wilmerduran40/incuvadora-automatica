# Incubadora Automatica - Resumen de Cambios

## Problemas originales
1. **Relay se bloquea** - prende, apaga y reinicia el sistema
2. **Bot Telegram no responde** comandos desde el chat
3. **Error `task not found`** inundaba el monitor serial
4. **Relay humificador (INT2) parpadea y falla modulo** - Causa: hardware del humificador en corto circuito

---

## Notas importantes

### Hardware - Corto circuito en humificador (Resuelto)
El modulo de humificador estaba en **corto circuito**, lo que causaba exceso de corriente en el relay INT2 (GPIO 26), provocando parpadeo rapido y fallo del modulo de relay.

**Solucion:** Reparar/reemplazar el modulo de humificador. Las protecciones de software (minimo 60s ON) se mantienen como barrera adicional.

### Conexiones electricas verificadas
- **GND compartido:** ESP32 ↔ Modulo relay (OBLIGATORIO)
- **Fuente relay:** Externa 5V separada del ESP32
- **Pines:** GPIO25 (Cal), GPIO26 (Hum), GPIO27 (Motor)

---

## Cambios realizados

### 1. Watchdog eliminado
**Archivo:** `incubadora.ino`

- Eliminado `#include <esp_task_wdt.h>`
- Eliminada configuracion del watchdog en `setup()`
- Eliminadas todas las llamadas a `esp_task_wdt_reset()` (5 en total)

**Causa:** El watchdog estaba configurado a 5s pero la tarea Arduino (`loopTask`) nunca se registro en el, causando errores `task not found` que inundaban el serial.

### 2. Debounce a calefactor y humificador
**Constante:** `DEBOUNCE_ACTUADOR_MS = 10000` (10 segundos)

- `controlarCalefactor()` - No puede cambiar estado si no pasaron 10s desde el ultimo cambio
- `controlarHumificador()` - Misma proteccion

### 3. Tiempo minimo ON para humificador
**Constante:** `MIN_ON_HUM_MS = 60000` (60 segundos)

- `controlarHumificador()` - El relay de humedad debe permanecer encendido minimo 60 segundos antes de poder apagarse
- Previene parpadeo por oscilacion de humedad en el umbral

### 4. Diagnostico de toggleHumManual()
- Imprime estado del PIN despues de cada cambio: `HUM MANUAL: ON → RELAY LOW (encendido)`
- Util para verificar en monitor serial que el relay responde correctamente

**Causa:** La ventana de histereis era muy estrecha (0.3°C), causando parpadeo del relay cuando la temperatura oscilaba en el limite.

### 3. Cooldown a alertas de Telegram
**Constante:** `COOLDOWN_ALERTA_MS = 60000` (60 segundos)

- `verificarAlertas()` - No envia la misma alerta mas de una vez por minuto

### 4. Separacion WiFi / Relay
**Variable:** `wifiEstable`

- Los actuadores NO se activan hasta que WiFi conecte O pasen 15s sin WiFi
- Esto evita que el pico de corriente del WiFi y el del relay ocurran al mismo tiempo

### 5. Retardo de actuadores
**Constante:** `DELAY_ACTUADORES_MS = 8000` (8 segundos, era 2s)

- Mayor margen antes de activar relays tras el arranque

### 6. Diagnostico de reset
En `setup()`, se imprime la razon del reinicio:
```
Razon de reset: ENCENDIDO / power-on
Razon de reset: BROWNOUT (bajo voltaje)
Razon de reset: WATCHDOG DE TAREA (WDT)
Razon de reset: PANIC / exception
```

### 7. NTP con reintentos
- 3 servidores NTP: `pool.ntp.org`, `time.nist.gov`, `time.google.com`
- Reintenta cada 60 segundos si no sincroniza
- Diagnostico cada 10 segundos: `NTP: hora = X`

### 8. Trace de relays
Cada cambio de estado imprime en serial:
```
RELAY CAL ON @ 35.6C (<37.5C)
RELAY CAL OFF @ 38.2C (>37.8C)
RELAY HUM ON @ 48% (<50%)
RELAY HUM OFF @ 56% (>55%)
```

---

## Bot de Telegram con botones inline

### Menu principal
Al enviar `/start` o `/menu`:

```
INCUBADORA
Temp: 35.6C (obj 37.6C)
Hum: 62%
Cal: ON | Hum: OFF
Dia: 3/21

Que quieres hacer?

[Temp +0.5] [Temp -0.5]
[Calefactor] [Humificador]
[Volteo] [Info]
[Parametros]
```

### Botones disponibles

| Boton | callback_data | Accion |
|-------|---------------|--------|
| Temp +0.5 | `temp_up` | Sube temp objetivo 0.5°C |
| Temp -0.5 | `temp_down` | Baja temp objetivo 0.5°C |
| Calefactor | `cal_toggle` | Toggle modo manual calefactor |
| Humificador | `hum_toggle` | Toggle modo manual humificador |
| Volteo | `volteo` | Fuerza volteo de huevos |
| Info | `info` | Muestra estado completo |
| Parametros | `params` | Muestra todos los parametros |

### Comandos de texto

| Comando | Descripcion |
|---------|-------------|
| `/start` | Muestra menu con botones |
| `/menu` | Muestra menu con botones |
| `/info` | Estado completo + menu |
| `/help` | Ayuda |
| `/id` | Tu chat_id |

### Menu editable (sin llenar el chat)

El menu se **edita** en lugar de enviar mensajes nuevos. Cada vez que presionas un boton, el mismo mensaje se actualiza con el estado nuevo. El chat queda limpio.

### Registro de comandos en Telegram
```json
[
  {"command": "start", "description": "Iniciar"},
  {"command": "menu", "description": "Menu principal"},
  {"command": "info", "description": "Estado completo"},
  {"command": "help", "description": "Ayuda"},
  {"command": "id", "description": "Tu chat_id"}
]
```

---

## Constantes importantes

| Constante | Valor | Descripcion |
|-----------|-------|-------------|
| `DELAY_ACTUADORES_MS` | 8000 | Tiempo minimo antes de activar relays |
| `STAGGER_RELAY_MS` | 150 | Tiempo entre cambios de relay |
| `DEBOUNCE_ACTUADOR_MS` | 10000 | Tiempo minimo entre cambios de relay |
| `COOLDOWN_ALERTA_MS` | 60000 | Tiempo minimo entre alertas Telegram |
| `MOTOR_TIMEOUT_MS` | 30000 | Timeout del motor de volteo |
| `INTERVALO_LECTURA` | 2000 | Intervalo de lectura del sensor |
| `INTERVALO_PANTALLA` | 1000 | Intervalo de actualizacion de pantalla |
| `INTERVALO_BOT` | 5000 | Intervalo de polling de Telegram |
| `INTERVALO_GUARDADO` | 60000 | Intervalo de guardado en EEPROM |

---

## Pines

| Pin | Funcion |
|-----|---------|
| GPIO 4 | DHT22 (Sensor temperatura/humedad) |
| GPIO 25 | Relay Calefactor (IN1) |
| GPIO 26 | Relay Humificador (IN2) |
| GPIO 27 | Relay Motor (IN3) |
| GPIO 16 | TFT CS |
| GPIO 5 | TFT DC |
| GPIO 18 | TFT CLK (SPI) |
| GPIO 23 | TFT MOSI (SPI) |
| GPIO 17 | TFT RST |

---

## Diagnostico rapido

Si el sistema se reinicia, buscar en el monitor serial:

1. `Razon de reset: BROWNOUT` → Fuente de poder insuficiente
2. `Razon de reset: WATCHDOG DE TAREA` → Algo bloquea el loop >10s
3. `Razon de reset: PANIC` → Crash por codigo
4. `Razon de reset: ENCENDIDO` → Primer arranque normal

Si el bot no responde, verificar:
1. `Hora NTP sincronizada.` aparece en el serial
2. Token configurado con `token TU_TOKEN`
3. Chat_id autorizado con `allow TU_CHAT_ID`
