#include <Arduino.h>

// Practica 8: Control proporcional de temperatura con ESP32, DS18B20, keypad 4x4, LCD I2C y servo
// El servo gira la perilla del dimmer del foco (calienta); Peltier y extractor van con relevador.
// A = SP | B = Kp | C = borrar | D = salir | * = punto decimal / cambiar pantalla | # = confirmar

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

#define PIN_DS18B20  4
#define PIN_SERVO    18
#define LCD_SDA      21
#define LCD_SCL      22
#define RELE_PELTIER    27   // celda Peltier + su ventilador (enfria)
#define RELE_EXTRACTOR  13   // extractor

// Modulo de relevadores activo en bajo. Si funcionan al reves, intercambia LOW y HIGH.
#define RELE_ON  LOW
#define RELE_OFF HIGH

const byte FILAS = 4;
const byte COLUMNAS = 4;
char teclas[FILAS][COLUMNAS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte pinesFilas[FILAS]       = {19, 23, 16, 17};  // R1, R2, R3, R4
byte pinesColumnas[COLUMNAS] = {26, 25, 33, 32};  // C1, C2, C3, C4

const int ANGULO_MIN = 90;              // dimmer al minimo (calibrar)
const int ANGULO_MAX = 180;             // dimmer al maximo (calibrar)
const float SP_MIN = 35.0, SP_MAX = 40.0;
const float KP_MIN = 0.0,  KP_MAX = 50.0;
const float TEMP_SEGURIDAD = 60.0;      // C: arriba de esto se apaga el foco
const float HIST_PELTIER = 0.5;         // C arriba del SP para encender la Peltier
const bool EXTRACTOR_SIEMPRE = true;    // false: el extractor solo acompana a la Peltier
const unsigned long TS_MS = 1000;       // periodo de muestreo
const unsigned long AVISO_MS = 1500;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Keypad teclado = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, FILAS, COLUMNAS);
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensor(&oneWire);
Servo servoDimmer;

float setpoint = 38.0;
float kp = 10.0;                        // % de potencia por cada grado de error
float temperatura = NAN;
float error = 0.0;
float errorPct = 0.0;                   // error en % del SP
float salida = 0.0;                     // 0 a 100 %
int angulo = ANGULO_MIN;
bool sensorOk = false;
bool peltierOn = false;

enum Captura { NINGUNA, CAPTURA_SP, CAPTURA_KP };
Captura captura = NINGUNA;
String entrada = "";

unsigned long tControl = 0;
unsigned long tAviso = 0;
int pantalla = 0;
char aviso1[17] = "";
char aviso2[17] = "";

const char GRADO = (char)223;

void imprimirLinea(int fila, const char* texto) {
  lcd.setCursor(0, fila);
  int n = 0;
  while (texto[n] != '\0' && n < 16) lcd.print(texto[n++]);
  while (n++ < 16) lcd.print(' ');
}

void mostrarAviso(const char* l1, const char* l2) {
  snprintf(aviso1, sizeof(aviso1), "%s", l1);
  snprintf(aviso2, sizeof(aviso2), "%s", l2);
  tAviso = millis();
}

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

  if (sensorOk) {
    error = setpoint - temperatura;
    errorPct = error / setpoint * 100.0;
  }
  controlarRelevadores();

  if (!sensorOk || peltierOn) salida = 0.0;            // al enfriar el foco queda al minimo
  else salida = constrain(kp * error, 0.0, 100.0);     // error negativo = 0 %

  int nuevoAngulo = ANGULO_MIN + (int)round(salida / 100.0 * (ANGULO_MAX - ANGULO_MIN));
  if (nuevoAngulo != angulo) {
    angulo = nuevoAngulo;
    servoDimmer.write(angulo);
  }

  // t(s), T, SP, error, error(%), salida(%), angulo, peltier
  Serial.printf("%lu,%.2f,%.1f,%.2f,%.1f,%.1f,%d,%d\n", millis() / 1000, sensorOk ? temperatura : NAN,
                setpoint, error, errorPct, salida, angulo, peltierOn);
}

void iniciarCaptura(Captura tipo) {
  captura = tipo;
  entrada = "";
}

void confirmarCaptura() {
  if (entrada.length() == 0 || entrada == ".") {
    captura = NINGUNA;
    return;
  }
  float valor = entrada.toFloat();
  if (captura == CAPTURA_SP) {
    if (valor < SP_MIN || valor > SP_MAX) {
      mostrarAviso("SP fuera rango", "Usa 35 a 40");
    } else {
      setpoint = valor;
      mostrarAviso("SP guardado", "");
    }
  } else if (captura == CAPTURA_KP) {
    if (valor < KP_MIN || valor > KP_MAX) {
      mostrarAviso("Kp fuera rango", "Usa 0 a 50");
    } else {
      kp = valor;
      mostrarAviso("Kp guardado", "");
    }
  }
  captura = NINGUNA;
}

void leerTeclado() {
  char tecla = teclado.getKey();
  if (!tecla) return;

  if (tecla == 'A') { iniciarCaptura(CAPTURA_SP); return; }
  if (tecla == 'B') { iniciarCaptura(CAPTURA_KP); return; }
  if (tecla == 'D') { captura = NINGUNA; return; }
  if (captura == NINGUNA) {
    if (tecla == '*') pantalla = 1 - pantalla;
    return;
  }

  if (tecla == 'C') {
    entrada = "";
  } else if (tecla == '#') {
    confirmarCaptura();
  } else if (tecla == '*') {
    if (entrada.indexOf('.') < 0 && entrada.length() < 5) entrada += '.';
  } else if (entrada.length() < 5) {
    entrada += tecla;
  }
}

void actualizarLCD() {
  char l1[24], l2[24];

  if (millis() - tAviso < AVISO_MS) {
    imprimirLinea(0, aviso1);
    imprimirLinea(1, aviso2);
    return;
  }

  if (captura != NINGUNA) {
    snprintf(l1, sizeof(l1), "%s %s_", captura == CAPTURA_SP ? "Nuevo SP:" : "Nuevo Kp:", entrada.c_str());
    imprimirLinea(0, l1);
    imprimirLinea(1, "#OK C:Borr D:Sal");
    return;
  }

  if (pantalla == 0) {
    if (sensorOk) snprintf(l1, sizeof(l1), "T:%.1f%c SP:%.1f%c", temperatura, GRADO, setpoint, GRADO);
    else          snprintf(l1, sizeof(l1), "Error DS18B20");
    snprintf(l2, sizeof(l2), "E:%.1f%% OUT:%d%%", fabs(errorPct) < 0.05 ? 0.0 : errorPct, (int)round(salida));
  } else {
    snprintf(l1, sizeof(l1), "Kp:%.1f SER:%d%c", kp, angulo, GRADO);
    snprintf(l2, sizeof(l2), "Pelt:%-3s Ext:%s", peltierOn ? "ON" : "OFF",
             (EXTRACTOR_SIEMPRE || peltierOn) ? "ON" : "OFF");
  }
  imprimirLinea(0, l1);
  imprimirLinea(1, l2);
}

void setup() {
  Serial.begin(115200);

  pinMode(RELE_PELTIER, OUTPUT);
  pinMode(RELE_EXTRACTOR, OUTPUT);
  digitalWrite(RELE_PELTIER, RELE_OFF);
  digitalWrite(RELE_EXTRACTOR, EXTRACTOR_SIEMPRE ? RELE_ON : RELE_OFF);

  servoDimmer.setPeriodHertz(50);
  servoDimmer.attach(PIN_SERVO, 500, 2400);
  servoDimmer.write(ANGULO_MIN);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  imprimirLinea(0, "Control P");
  imprimirLinea(1, "Iniciando...");

  sensor.begin();
  sensor.setWaitForConversion(true);
  sensor.requestTemperatures();          // primera lectura completa
  sensor.setWaitForConversion(false);

  Serial.println("t,T,SP,error,error_pct,salida,angulo,peltier");
  delay(1000);
}

void loop() {
  leerTeclado();

  if (millis() - tControl >= TS_MS) {
    tControl = millis();
    controlar();
  }

  static unsigned long tLCD = 0;
  if (millis() - tLCD >= 200) {
    tLCD = millis();
    actualizarLCD();
  }
}
