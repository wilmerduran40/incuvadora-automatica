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
