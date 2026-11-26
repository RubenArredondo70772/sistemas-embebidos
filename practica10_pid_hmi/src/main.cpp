#include <Arduino.h>

// Practica 10: Control PID de temperatura con ESP32, DS18B20 y HMI Nextion NX3224F028
// El servo gira la perilla del dimmer del foco; Peltier y extractor encendidos con relevador.
// Nextion -> ESP32: 0x52 = redibujar grafica | 0x55 + 16 bytes = SP, Kp, Ki, Kd (enteros de 32 bits)

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// ---------------- PINES ----------------
#define PIN_DS18B20      4
#define PIN_SERVO        18
#define RELAY_PELTIER    19
#define RELAY_EXTRACTOR  23
#define NEXTION_RX       16   // RX2 del ESP32 <- TX de la Nextion
#define NEXTION_TX       17   // TX2 del ESP32 -> RX de la Nextion

// Si tus reles funcionan al reves, cambia LOW por HIGH.
#define RELAY_ACTIVO   LOW
#define RELAY_INACTIVO (!RELAY_ACTIVO)
const bool EXTRACTOR_SIEMPRE_ON = true;

const unsigned long NEXTION_BAUD = 9600;   // debe coincidir con baud= en Program.s

// ---------------- DIMMER (calibracion) ----------------
const int ANGULO_MIN = 55;    // 0 % de luz
const int ANGULO_MAX = 180;   // 100 % de luz

// ---------------- SEGURIDAD Y LIMITES ----------------
const float TEMP_SEGURIDAD = 80.0;   // C: arriba de esto el foco se apaga
const float SP_MAX = 75.0;
const float KP_MAX = 100.0;
const float KI_MAX = 10.0;
const float KD_MAX = 100.0;

// ---------------- GRAFICA ----------------
const int   ID_GRAFICA   = 8;      // id de w_graf
const int   ALTO_GRAFICA = 135;    // h de w_graf
const float T_MIN_G      = 20.0;   // temperatura en el fondo de la grafica
const float T_MAX_G      = 50.0;   // temperatura en el tope de la grafica
const int   CICLOS_POR_PUNTO = 1;  // 1 punto cada N segundos (2 o 3 = mas historia)
const int   HIST_N       = 120;    // puntos guardados (con 115200 baudios puedes subir a 238)

uint8_t histT[HIST_N];
uint8_t histS[HIST_N];
unsigned long totalPuntos = 0;     // puntos guardados desde el arranque
bool redibujando = false;
unsigned long redibujoSig = 0;
int contadorPunto = 0;

// ---------------- OBJETOS ----------------
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensores(&oneWire);
Servo dimmer;
Preferences prefs;

// ---------------- PARAMETROS PID ----------------
float setpoint = 40.0;
float kp = 10.0;
float ki = 0.0;     // arranca en 0: ve subiendolo con la pantalla
float kd = 0.0;

float actual = 0.0;
float error  = 0.0;
float termP = 0.0, termI = 0.0, termD = 0.0;
float u = 0.0;                 // salida 0..100 %
int   angulo = ANGULO_MIN;

float tempAnterior = 0.0;
bool  hayTempAnterior = false;
unsigned long tUltimoPID = 0;
bool  sensorOK = false;
bool  sobretemp = false;

// ---------------- LECTURA NO BLOQUEANTE DEL DS18B20 ----------------
const unsigned long PERIODO_PID_MS = 1000;
const unsigned long ESPERA_CONVERSION_MS = 800;
unsigned long tPeticion = 0;
bool esperando = false;

// ---------------- ESTADO EN PANTALLA (t_estado, opcional) ----------------
char estadoActual[16] = "";

bool paramsEnviados1 = false;
bool paramsEnviados2 = false;

// =====================================================================
// NEXTION
void nxCmd(const char* cmd) {
  Serial2.print(cmd);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
}

void nxNumero(const char* nombre, long valor) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s.val=%ld", nombre, valor);
  nxCmd(buf);
}

void nxTexto(const char* nombre, const char* texto) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s.txt=\"%s\"", nombre, texto);
  nxCmd(buf);
}

void nxPunto(int canal, int y) {
  char buf[32];
  snprintf(buf, sizeof(buf), "add %d,%d,%d", ID_GRAFICA, canal, y);
  nxCmd(buf);
}

void setEstado(const char* txt) {
  if (strcmp(estadoActual, txt) == 0) return;
  strncpy(estadoActual, txt, sizeof(estadoActual) - 1);
  estadoActual[sizeof(estadoActual) - 1] = '\0';
  nxTexto("t_estado", estadoActual);   // si no existe ese texto, se ignora
}

// Manda a la pagina "config" los valores guardados
void enviarParametrosAPantalla() {
  nxNumero("cx_sp", lround(setpoint * 10));
  nxNumero("cx_kp", lround(kp * 10));
  nxNumero("cx_ki", lround(ki * 100));
  nxNumero("cx_kd", lround(kd * 10));
}

int aPixeles(float valorC) {
  int y = (int)((valorC - T_MIN_G) * ALTO_GRAFICA / (T_MAX_G - T_MIN_G));
  return constrain(y, 0, ALTO_GRAFICA - 1);
}

void guardarHistoria(int yTemp, int ySet) {
  int k = totalPuntos % HIST_N;
  histT[k] = (uint8_t)yTemp;
  histS[k] = (uint8_t)ySet;
  totalPuntos++;
}

void iniciarRedibujo() {
  char buf[24];
  snprintf(buf, sizeof(buf), "cle %d,255", ID_GRAFICA);
  nxCmd(buf);
  redibujoSig = (totalPuntos > HIST_N) ? totalPuntos - HIST_N : 0;
  redibujando = true;
  estadoActual[0] = '\0';   // fuerza a reenviar el estado
  setEstado(sensorOK ? (sobretemp ? "SOBRETEMP" : "OK") : "ERROR SENSOR");
}

// Redibuja la historia poco a poco, sin bloquear el control
void pasoRedibujo() {
  if (!redibujando) return;
  int enviados = 0;
  while (enviados < 6 && Serial2.availableForWrite() >= 200) {
    unsigned long minimo = (totalPuntos > HIST_N) ? totalPuntos - HIST_N : 0;
    if (redibujoSig < minimo) redibujoSig = minimo;
    if (redibujoSig >= totalPuntos) { redibujando = false; break; }
    int k = redibujoSig % HIST_N;
    nxPunto(0, histT[k]);
    nxPunto(1, histS[k]);
    redibujoSig++;
    enviados++;
  }
}

void guardarConfiguracion() {
  prefs.begin("horno", false);
  prefs.putFloat("sp", setpoint);
  prefs.putFloat("kp", kp);
  prefs.putFloat("ki", ki);
  prefs.putFloat("kd", kd);
  prefs.end();
}

void leerParametros() {
  byte d[16];
  int n = 0;
  unsigned long t0 = millis();
  while (n < 16 && millis() - t0 < 200) {
    if (Serial2.available()) d[n++] = Serial2.read();
  }
  if (n < 16) return;   // paquete incompleto: se ignora

  int32_t v[4];
  for (int i = 0; i < 4; i++) {
    v[i] = (int32_t)((uint32_t)d[4 * i] |
                     ((uint32_t)d[4 * i + 1] << 8) |
                     ((uint32_t)d[4 * i + 2] << 16) |
                     ((uint32_t)d[4 * i + 3] << 24));
  }

  setpoint = constrain(v[0] / 10.0,  0.0, SP_MAX);
  kp       = constrain(v[1] / 10.0,  0.0, KP_MAX);
  ki       = constrain(v[2] / 100.0, 0.0, KI_MAX);
  kd       = constrain(v[3] / 10.0,  0.0, KD_MAX);
  termI = 0.0;   // evita saltos del servo al cambiar parametros
  guardarConfiguracion();

  Serial.print("APLICADO -> SP:"); Serial.print(setpoint, 1);
  Serial.print(" Kp:"); Serial.print(kp, 1);
  Serial.print(" Ki:"); Serial.print(ki, 2);
  Serial.print(" Kd:"); Serial.println(kd, 1);
}

void leerNextion() {
  while (Serial2.available()) {
    byte b = Serial2.read();
    if (b == 0x52)      iniciarRedibujo();
    else if (b == 0x55) leerParametros();
  }
}

// =====================================================================
// PID
void reiniciarPID() {
  termP = 0.0; termI = 0.0; termD = 0.0;
  hayTempAnterior = false;
}

void aplicarServo() {
  float grados = ANGULO_MIN + (u / 100.0) * (ANGULO_MAX - ANGULO_MIN);
  angulo = constrain((int)(grados + 0.5), ANGULO_MIN, ANGULO_MAX);
  dimmer.write(angulo);
}

void enviarPantalla() {
  nxNumero("x_vp",  lround(actual * 10));
  nxNumero("x_sp",  lround(setpoint * 10));
  nxNumero("x_err", lround(error * 10));
  nxNumero("n_out", lround(u));

  contadorPunto++;
  if (contadorPunto >= CICLOS_POR_PUNTO) {
    contadorPunto = 0;
    int yT = aPixeles(actual);
    int yS = aPixeles(setpoint);
    guardarHistoria(yT, yS);
    if (!redibujando) {   // durante el redibujo el punto se manda desde la historia
      nxPunto(0, yT);
      nxPunto(1, yS);
    }
  }
}

void falloSensor() {
  sensorOK = false;
  reiniciarPID();
  u = 0.0;
  aplicarServo();               // foco apagado (ANGULO_MIN)
  nxNumero("n_out", 0);
  setEstado("ERROR SENSOR");
  Serial.println("ERROR: DS18B20 no responde, foco apagado");
}

void pasoPID(float t) {
  unsigned long ahora = millis();
  float Ts = (tUltimoPID == 0) ? 1.0 : (ahora - tUltimoPID) / 1000.0;
  tUltimoPID = ahora;
  if (Ts <= 0.0) Ts = 1.0;

  actual = t;
  error = setpoint - actual;

  // Proteccion por sobretemperatura (con histeresis de 2 C)
  if (sobretemp) { if (actual <= TEMP_SEGURIDAD - 2.0) sobretemp = false; }
  else if (actual >= TEMP_SEGURIDAD) sobretemp = true;

  if (sobretemp) {
    reiniciarPID();
    u = 0.0;
    setEstado("SOBRETEMP");
  } else {
    setEstado("OK");

    termP = kp * error;

    // Derivada sobre la temperatura medida
    if (hayTempAnterior) termD = -kd * (actual - tempAnterior) / Ts;
    else                 termD = 0.0;
    tempAnterior = actual;
    hayTempAnterior = true;

    // Integral con anti-windup (integracion condicional + limite 0..100)
    float integralNueva = termI + ki * error * Ts;
    float uPrueba = termP + integralNueva + termD;
    bool satArriba = (uPrueba > 100.0 && error > 0);
    bool satAbajo  = (uPrueba < 0.0   && error < 0);
    if (!satArriba && !satAbajo) termI = integralNueva;
    termI = constrain(termI, 0.0, 100.0);

    u = constrain(termP + termI + termD, 0.0, 100.0);   // nunca negativa
  }

  aplicarServo();
  enviarPantalla();

  // CSV para Excel
  Serial.print(ahora / 1000.0, 1); Serial.print(",");
  Serial.print(setpoint, 1);       Serial.print(",");
  Serial.print(actual, 2);         Serial.print(",");
  Serial.print(error, 2);          Serial.print(",");
  Serial.print(termP, 1);          Serial.print(",");
  Serial.print(termI, 1);          Serial.print(",");
  Serial.print(termD, 1);          Serial.print(",");
  Serial.print(u, 1);              Serial.print(",");
  Serial.println(angulo);
}

// =====================================================================
// SENSOR
void leerSensor() {
  unsigned long ahora = millis();

  if (!esperando && ahora - tPeticion >= PERIODO_PID_MS) {
    sensores.requestTemperatures();
    tPeticion = ahora;
    esperando = true;
  }

  if (esperando && ahora - tPeticion >= ESPERA_CONVERSION_MS) {
    esperando = false;
    float t = sensores.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C || t < -50 || t > 125) {
      falloSensor();
    } else {
      sensorOK = true;
      pasoPID(t);
    }
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);

  // Peltier y extractor encendidos desde el arranque
  digitalWrite(RELAY_PELTIER, RELAY_ACTIVO);
  digitalWrite(RELAY_EXTRACTOR, EXTRACTOR_SIEMPRE_ON ? RELAY_ACTIVO : RELAY_INACTIVO);
  pinMode(RELAY_PELTIER, OUTPUT);
  pinMode(RELAY_EXTRACTOR, OUTPUT);

  // Servo: arranca en 0 % de luz
  dimmer.setPeriodHertz(50);
  dimmer.attach(PIN_SERVO, 500, 2400);
  dimmer.write(ANGULO_MIN);

  // Nextion en Serial2
  Serial2.setTxBufferSize(512);
  Serial2.begin(NEXTION_BAUD, SERIAL_8N1, NEXTION_RX, NEXTION_TX);

  // DS18B20
  sensores.begin();
  sensores.setResolution(12);
  sensores.setWaitForConversion(false);

  // Valores guardados
  prefs.begin("horno", false);
  setpoint = constrain(prefs.getFloat("sp", 40.0), 0.0, SP_MAX);
  kp       = constrain(prefs.getFloat("kp", 10.0), 0.0, KP_MAX);
  ki       = constrain(prefs.getFloat("ki", 0.0),  0.0, KI_MAX);
  kd       = constrain(prefs.getFloat("kd", 0.0),  0.0, KD_MAX);
  prefs.end();

  Serial.println("=== Control PID del horno (ESP32 + Nextion) ===");
  Serial.print("SP:"); Serial.print(setpoint, 1);
  Serial.print(" Kp:"); Serial.print(kp, 1);
  Serial.print(" Ki:"); Serial.print(ki, 2);
  Serial.print(" Kd:"); Serial.println(kd, 1);
  Serial.println("t_s,SP,T,E,P,I,D,U,Servo");
}

void loop() {
  leerNextion();
  leerSensor();
  pasoRedibujo();

  // Mandar a la pantalla los valores guardados (2 veces, por si la pantalla arranca despues)
  if (!paramsEnviados1 && millis() > 2000) { enviarParametrosAPantalla(); paramsEnviados1 = true; }
  if (!paramsEnviados2 && millis() > 6000) { enviarParametrosAPantalla(); paramsEnviados2 = true; }
}