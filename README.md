# Incubadora Automatica v2.0 - ESP32

## Mapeo de Conexiones

### Pantalla TFT ST7789 (170x320 IPS)

| Pin Pantalla | Pin ESP32 | Función       |
|-------------|-----------|---------------|
| BLK         | 3.3V      | Backlight     |
| DC          | GPIO 16   | Data/Command  |
| CS          | GPIO 5    | Chip Select   |
| RES         | GPIO 17   | Reset         |
| SDA         | GPIO 23   | Data SPI (MOSI) |
| SCL         | GPIO 18   | Clock SPI (SCK) |
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

## Comandos Serial (115200 baud)
| Comando | Acción                     |
|---------|----------------------------|
| +/-     | Ajustar temp objetivo      |
| h       | Toggle humificador manual  |
| t       | Volteo forzado             |
| s       | Guardar estado en flash    |
| d       | Mostrar info               |
| r       | Reset (borrar estado)      |
| c       | Toggle calefactor manual   |
