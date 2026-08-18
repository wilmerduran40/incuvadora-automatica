# Plan de mejoras futuras - Incubadora Automatica

> Este plan esta guardado para ejecutarse **despues de confirmar que el firmware minimo estable funciona sin problemas** durante varios dias de operacion continua.

---

## Fase 1: Configurar parametros del perfil por Telegram

### Objetivo
Permitir ajustar el perfil personalizado desde Telegram sin arriesgar la incubacion activa.

### Opciones de seguridad a definir

- **Opcion A - Confianza al chat autorizado:**
  - Comandos directos: `set tobj 37.6`, `set tmin 37.5`, `set dias 21`, etc.
  - Mas comodo, pero menos seguro si el chat se ve comprometido.

- **Opcion B - PIN de seguridad (recomendada para comercializar):**
  - Primero enviar: `config 1234`
  - Si el PIN es correcto, se abre una ventana de 10 minutos para hacer cambios.
  - El PIN se configura por serial o por portal AP.

- **Opcion C - Confirmacion fisica:**
  - El cambio por Telegram queda pendiente.
  - Debe confirmarse con un boton fisico en el dispositivo o con un comando serial local.
  - La maxima seguridad, pero menos practica a distancia.

### Implementacion tecnica propuesta

1. Recrear una cola ligera `QueueHandle_t` para pasar comandos desde la tarea/logica de Telegram al `loop()` sin bloquear el control.
2. Validar rangos de seguridad antes de aplicar:
   - Temperatura objetivo: 35.0 a 40.0 °C
   - Temperatura min/max: +/- 1.5 °C del objetivo como maximo
   - Humedad: 30 a 80 %
   - Dias totales: 10 a 60
   - Lockdown: menor que dias totales
   - Intervalo volteo: 1 a 12 horas
   - Duracion volteo: 5 a 60 segundos
3. Guardar automaticamente en NVS despues de cada cambio valido.
4. Enviar mensaje de confirmacion con los valores aplicados.
5. Registrar cada cambio por serial con timestamp y chat_id.

### Comandos de Telegram propuestos

```
set tobj 37.6     temperatura objetivo
set tmin 37.5     temperatura minima
set tmax 37.8     temperatura maxima
set hmin 50       humedad min desarrollo
set hmax 55       humedad max desarrollo
set hlmin 65      humedad min lockdown
set hlmax 70      humedad max lockdown
set dias 21       dias totales
set lock 18       dia lockdown
set vol 2         intervalo volteo (horas)
set dur 20        duracion volteo (segundos)
params            ver parametros actuales
```

---

## Fase 2: Modo AP para configuracion inicial

### Objetivo
Que un usuario sin conocimientos tecnicos pueda configurar WiFi, token y parametros iniciales sin cable serial.

### Diseno robusto

- **Activacion solo por boton fisico:**
  - Pulsar un boton (por ejemplo GPIO 0) durante 5 segundos al encender.
  - Evita que el ESP32 entre en modo AP accidentalmente.

- **Portal web minimo:**
  - Un solo formulario HTML embebido en PROGMEM.
  - Campos: SSID, clave WiFi, token Telegram, dias totales, dia lockdown, temp objetivo.
  - Solo dos endpoints:
    - `/` muestra el formulario.
    - `/save` guarda y reinicia.

- **Sin DNS server cautivo:**
  - Para evitar saturacion de peticiones y bloqueos.
  - Mostrar la IP `192.168.4.1` en la pantalla TFT y en el nombre del AP.

- **Timeout de seguridad:**
  - Si nadie guarda configuracion en 5 minutos, salir de modo AP y volver a modo STA.

- **Indicador visual:**
  - Mostrar "MODO CONFIG" en la pantalla TFT.
  - Opcional: LED parpadeante mientras este en modo AP.

### Credenciales del AP temporal

```
SSID: Incubadora-Setup
Clave: incubadora123
IP: 192.168.4.1
```

---

## Fase 3: Consideraciones para comercializar

### Hardware

- Caja aislante con proteccion IP adecuada para ambiente humedo.
- Bornes protegidos para conexiones de 110V.
- Fusible en la entrada de alimentacion principal.
- Etiquetado de advertencia de alto voltaje.
- Fuente de alimentacion certificada y estable.
- PCB con separacion clara entre parte de bajo voltaje (ESP32, sensor, TFT) y alto voltaje (relays, actuadores).

### Experiencia de usuario

- Etiqueta en el dispositivo con:
  - Nombre del AP de configuracion.
  - Clave del AP.
  - QR que lleva a video tutorial de configuracion.
- Manual corto en papel (maximo 2 paginas).
- Proceso de emparejamiento en 3 pasos: encender, conectar AP, configurar.

### Software

- Mantener el firmware base estable como version "LTS".
- Liberar nuevas funciones solo despues de pruebas de estres de al menos 7 dias continuos.
- Incluir comando `diagnostico` en Telegram para soporte remoto:
  - Heap libre.
  - Tiempo de funcionamiento.
  - Estado del sensor.
  - Historial de errores.
- Opcional: app movil propia si Telegram resulta poco intuitivo para el usuario final.

### Soporte y garantia

- Definir politica de garantia clara.
- Tener version de firmware anterior estable por si hay que hacer downgrade.
- Canal de soporte tecnico (WhatsApp, Telegram o correo).

---

## Orden recomendado de implementacion

1. Confirmar que el firmware minimo estable funciona sin bloqueos durante varios dias.
2. Implementar **Fase 1** (Telegram para configurar parametros) con la opcion de seguridad elegida.
3. Probar la Fase 1 durante al menos 3 dias.
4. Implementar **Fase 2** (modo AP con boton fisico).
5. Probar la Fase 2 completamente: configurar desde cero varias veces.
6. Evaluar **Fase 3** (comercializacion) solo cuando el producto sea 100 % estable.

---

## Decisiones pendientes

Antes de empezar la Fase 1, definir:

- [ ] Opcion de seguridad para cambios por Telegram (A, B o C).
- [ ] Boton fisico para modo AP: ¿que GPIO usar?
- [ ] ¿Mantener Telegram como interfaz principal o desarrollar app movil?
- [ ] Rangos de seguridad finales para cada parametro.
