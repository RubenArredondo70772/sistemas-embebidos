// Practica 10: Control PID de temperatura con ESP32, DS18B20 y HMI Nextion
// El servo gira la perilla del dimmer del foco (calienta); Peltier y extractor van con relevador.

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

#define PIN_DS18B20      4
#define PIN_SERVO        18
#define RELE_PELTIER     27   // celda Peltier + su ventilador (enfria)
#define RELE_EXTRACTOR   13   // extractor
#define NEXTION_RX       16   // RX2 <- TX de la Nextion
#define NEXTION_TX       17   // TX2 -> RX de la Nextion

// Modulo de relevadores activo en bajo. Si funcionan al reves, intercambia LOW y HIGH.
#define RELE_ON  LOW
#define RELE_OFF HIGH

const int ANGULO_MIN = 90;              // dimmer al minimo (calibrar)
const int ANGULO_MAX = 180;             // dimmer al maximo (calibrar)
const float SP_MIN = 35.0, SP_MAX = 40.0;
const float TEMP_SEGURIDAD = 60.0;      // C: arriba de esto se apaga el foco
const float HIST_PELTIER = 0.5;         // C arriba del SP para encender la Peltier
const bool EXTRACTOR_SIEMPRE = true;    // false: el extractor solo acompana a la Peltier
const float GRAFICA_MAX = 60.0;         // C que corresponden al tope de la grafica
const unsigned long TS_MS = 1000;       // periodo de muestreo
const float TS = TS_MS / 1000.0;

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensor(&oneWire);
Servo servoDimmer;

float setpoint = 38.0;
float kp = 12.0;
float ki = 0.8;
float kd = 2.0;

float temperatura = NAN;
float tempAnterior = NAN;
float error = 0.0;
float errorPct = 0.0;                   // error en % del SP
float accionI = 0.0;
float salida = 0.0;                     // 0 a 100 % de potencia del foco
int angulo = ANGULO_MIN;
bool sensorOk = false;
bool peltierOn = false;

unsigned long tControl = 0;
String bufNextion = "";
String bufSerial = "";

// ---------- Nextion ----------
void nxComando(const String& cmd) {
  Serial2.print(cmd);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
}

void nxTexto(const char* obj, const String& texto) {
  nxComando(String(obj) + ".txt=\"" + texto + "\"");
}

void enviarParametros() {
  nxTexto("tKp", String(kp, 2));
  nxTexto("tKi", String(ki, 2));
  nxTexto("tKd", String(kd, 2));
  nxTexto("tSPin", String(setpoint, 1));
}

void enviarVariables() {
  bool extractorOn = EXTRACTOR_SIEMPRE || peltierOn;
  nxTexto("tPV", sensorOk ? String(temperatura, 1) + " C" : String("Error"));
  nxTexto("tSP", String(setpoint, 1) + " C");
  nxTexto("tErr", String(fabs(errorPct) < 0.05 ? 0.0 : errorPct, 1) + " %");
  nxTexto("tOut", String(salida, 0) + " %");
  nxTexto("tAng", String(angulo) + " grados");
  nxTexto("tPel", peltierOn ? "ON" : "OFF");
  nxTexto("tExt", extractorOn ? "ON" : "OFF");
  nxTexto("tModo", peltierOn ? "Enfriando" : (salida > 0 ? "Calentando" : "Reposo"));

  int vPV = constrain((int)(temperatura / GRAFICA_MAX * 255.0), 0, 255);
  int vSP = constrain((int)(setpoint / GRAFICA_MAX * 255.0), 0, 255);
  if (sensorOk) nxComando("add s0.id,0," + String(vPV));
  nxComando("add s0.id,1," + String(vSP));
}

// Linea esperada: SET,kp,ki,kd,sp   (la manda el boton Aplicar de la HMI)
void procesarLinea(String linea) {
  linea.trim();
  if (!linea.startsWith("SET,")) return;

  float v[4];
  int inicio = 4;
  for (int i = 0; i < 4; i++) {
    int coma = linea.indexOf(',', inicio);
    String parte = (coma < 0) ? linea.substring(inicio) : linea.substring(inicio, coma);
    if (parte.length() == 0) { nxTexto("tMsg", "Faltan datos"); return; }
    v[i] = parte.toFloat();
    inicio = coma + 1;
    if (coma < 0 && i < 3) { nxTexto("tMsg", "Faltan datos"); return; }
  }

  if (v[0] < 0 || v[0] > 100 || v[1] < 0 || v[1] > 10 || v[2] < 0 || v[2] > 200) {
    nxTexto("tMsg", "Ganancia fuera de rango");
    return;
  }
  if (v[3] < SP_MIN || v[3] > SP_MAX) {
    nxTexto("tMsg", "SP de 35 a 40 C");
    return;
  }

  kp = v[0];
  ki = v[1];
  kd = v[2];
  setpoint = v[3];
  if (ki == 0.0) accionI = 0.0;
  nxTexto("tMsg", "Aplicado");
  enviarParametros();
  Serial.printf("Nuevos parametros: Kp=%.2f Ki=%.2f Kd=%.2f SP=%.1f\n", kp, ki, kd, setpoint);
}

void leerEntrada(Stream& puerto, String& buf) {
  while (puerto.available()) {
    char c = puerto.read();
    if (c == '\n' || c == '\r') {
      if (buf.length() > 0) procesarLinea(buf);
      buf = "";
    } else if (c >= 32 && c <= 126 && buf.length() < 60) {
      buf += c;                            // ignora los codigos 0xFF de la Nextion
    }
  }
}

// ---------- Control ----------
bool leerTemperatura() {
  float t = sensor.getTempCByIndex(0);
  sensor.requestTemperatures();          // pide la siguiente lectura (no bloquea)
  if (t == DEVICE_DISCONNECTED_C || t < -50.0 || t > 125.0) return false;
  temperatura = t;
  return true;
}

void controlarRelevadores() {
  if (!sensorOk) peltierOn = false;
  else if (temperatura >= setpoint + HIST_PELTIER || temperatura >= TEMP_SEGURIDAD) peltierOn = true;
  else if (temperatura <= setpoint) peltierOn = false;

  digitalWrite(RELE_PELTIER, peltierOn ? RELE_ON : RELE_OFF);
  digitalWrite(RELE_EXTRACTOR, (EXTRACTOR_SIEMPRE || peltierOn) ? RELE_ON : RELE_OFF);
}

void controlar() {
  sensorOk = leerTemperatura();
  controlarRelevadores();

  if (!sensorOk || temperatura >= TEMP_SEGURIDAD) {
    salida = 0.0;
    accionI = 0.0;
    tempAnterior = NAN;
  } else {
    error = setpoint - temperatura;
    errorPct = error / setpoint * 100.0;
    float accionP = kp * error;
    // derivada sobre la medicion: un cambio de SP no produce un salto
    float accionD = isnan(tempAnterior) ? 0.0 : -kd * (temperatura - tempAnterior) / TS;
    tempAnterior = temperatura;

    // anti-windup: no se integra si la salida ya esta saturada en ese sentido
    float integralNueva = accionI + ki * error * TS;
    float u = accionP + integralNueva + accionD;
    if (!((u > 100.0 && error > 0) || (u < 0.0 && error < 0))) accionI = integralNueva;
    accionI = constrain(accionI, 0.0, 100.0);

    salida = constrain(accionP + accionI + accionD, 0.0, 100.0);
    if (peltierOn) salida = 0.0;           // al enfriar el foco queda al minimo
  }

  int nuevoAngulo = ANGULO_MIN + (int)round(salida / 100.0 * (ANGULO_MAX - ANGULO_MIN));
  if (nuevoAngulo != angulo) {
    angulo = nuevoAngulo;
    servoDimmer.write(angulo);
  }

  enviarVariables();

  // t(s), T, SP, error, error(%), salida(%), angulo, peltier
  Serial.printf("%lu,%.2f,%.1f,%.2f,%.1f,%.1f,%d,%d\n", millis() / 1000,
                sensorOk ? temperatura : NAN, setpoint, error, errorPct, salida, angulo, peltierOn);
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, NEXTION_RX, NEXTION_TX);

  pinMode(RELE_PELTIER, OUTPUT);
  pinMode(RELE_EXTRACTOR, OUTPUT);
  digitalWrite(RELE_PELTIER, RELE_OFF);
  digitalWrite(RELE_EXTRACTOR, EXTRACTOR_SIEMPRE ? RELE_ON : RELE_OFF);

  servoDimmer.setPeriodHertz(50);
  servoDimmer.attach(PIN_SERVO, 500, 2400);
  servoDimmer.write(ANGULO_MIN);

  sensor.begin();
  sensor.setWaitForConversion(true);
  sensor.requestTemperatures();          // primera lectura completa
  sensor.setWaitForConversion(false);

  delay(500);
  nxComando("bkcmd=0");                  // la Nextion no responde a cada comando
  enviarParametros();
  nxTexto("tMsg", "Listo");

  Serial.println("t,T,SP,error,error_pct,salida,angulo,peltier");
}

void loop() {
  leerEntrada(Serial2, bufNextion);
  leerEntrada(Serial, bufSerial);        // tambien acepta SET,... desde el monitor serie

  if (millis() - tControl >= TS_MS) {
    tControl = millis();
    controlar();
  }
}
