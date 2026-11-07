#include <Arduino.h>

// Practica 5: Control ON-OFF de temperatura con potenciometros y LM35 (ESP32)
// MAX y MIN se ajustan con dos potenciometros; foco calienta, Peltier + extractor enfrian.

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define PIN_LM35          32   // salida (VOUT) del LM35, alimentado a 5V

#define RELAY_FOCO        26   // foco (calienta)
#define RELAY_PELTIER     27   // celda Peltier + su ventilador (enfria)
#define RELAY_EXTRACTOR   13   // extractor (solo al enfriar)

#define POT_MAX           34   // pata central del potenciometro MAX (extremos a 3.3V y GND)
#define POT_MIN           35   // pata central del potenciometro MIN (extremos a 3.3V y GND)

#define LCD_SDA           21
#define LCD_SCL           22

// Modulo de reles activo en bajo (LOW). Si funcionan al reves, cambia LOW por HIGH.
#define RELAY_ACTIVO   LOW
#define RELAY_INACTIVO (!RELAY_ACTIVO)

const int RANGO_MAX_INFERIOR = 25;  // C
const int RANGO_MAX_SUPERIOR = 80;  // C
const int RANGO_MIN_INFERIOR = 20;  // C
const int RANGO_MIN_SUPERIOR = 75;  // C
const int MARGEN_MINIMO      = 5;   // C entre MAX y MIN

// Si al girar a la derecha el valor BAJA, cambia a true
const bool INVERTIR_POT_MAX = false;
const bool INVERTIR_POT_MIN = false;

const unsigned long PAUSA_CAMBIO_MS = 1000;  // foco y Peltier apagados al cambiar
const int MAX_ERRORES_SENSOR = 3;

LiquidCrystal_I2C lcd(0x27, 16, 2);  // si no se ve nada, prueba 0x3F

enum EstadoCiclo { INICIANDO, CALENTANDO, ENFRIANDO, ERROR_SENSOR };
EstadoCiclo estado = INICIANDO;

int tempMax = 60;
int tempMin = 30;

float tempActual    = 0.0;
float porcentaje    = 0.0;
int   erroresSensor = 0;
bool  hayLectura    = false;

unsigned long momentoCambioEstado = 0;

unsigned long momentoLectura = 0;
const unsigned long INTERVALO_SENSOR_MS = 100;
const int MUESTRAS_FILTRO = 5;
float muestras[MUESTRAS_FILTRO];
int indiceMuestra = 0;
bool filtroListo = false;
const int MV_MAXIMO_VALIDO = 1500;  // mas de 1.5 V = sensor desconectado

float potMaxFiltrado = -1;
float potMinFiltrado = -1;
unsigned long ultimaLecturaPots = 0;
const unsigned long INTERVALO_POTS_MS = 50;

unsigned long ultimaActualizacionLCD = 0;
const unsigned long INTERVALO_LCD_MS = 300;

const char GRADO = (char)223;

int  leerPromedioADC(int pin);
int  valorConHisteresis(float filtrado, int actual, int rangoInf, int rangoSup, bool invertir);
void leerPotenciometros();
void leerSensor();
void actualizarCiclo();
void cambiarEstado(EstadoCiclo nuevo);
void aplicarSalidas();
void apagarFocoYPeltier();
void actualizarExtractor();
const char* nombreEstado();
void actualizarLCD();
void imprimirLinea(int fila, const char* texto);

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_FOCO, OUTPUT);
  pinMode(RELAY_PELTIER, OUTPUT);
  pinMode(RELAY_EXTRACTOR, OUTPUT);
  apagarFocoYPeltier();
  digitalWrite(RELAY_EXTRACTOR, RELAY_INACTIVO);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(POT_MAX, INPUT);
  pinMode(POT_MIN, INPUT);
  pinMode(PIN_LM35, INPUT);

  delay(500);
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  leerPotenciometros();

  Serial.println("=== Control de temperatura con potenciometros ===");
  Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");

  delay(1000);
  lcd.clear();
}

void loop() {
  if (millis() - ultimaLecturaPots >= INTERVALO_POTS_MS) {
    ultimaLecturaPots = millis();
    leerPotenciometros();
  }

  if (millis() - momentoLectura >= INTERVALO_SENSOR_MS) {
    momentoLectura = millis();
    leerSensor();
    actualizarCiclo();
  }

  if (millis() - ultimaActualizacionLCD >= INTERVALO_LCD_MS) {
    ultimaActualizacionLCD = millis();
    actualizarLCD();
  }
}

int leerPromedioADC(int pin) {
  const int MUESTRAS = 16;
  long suma = 0;
  for (int i = 0; i < MUESTRAS; i++) suma += analogRead(pin);
  return suma / MUESTRAS;
}

int valorConHisteresis(float filtrado, int actual, int rangoInf, int rangoSup, bool invertir) {
  float lectura = invertir ? 4095.0 - filtrado : filtrado;
  float grados = rangoInf + (lectura / 4095.0) * (rangoSup - rangoInf);
  if (fabs(grados - actual) >= 0.75) {
    return constrain((int)(grados + 0.5), rangoInf, rangoSup);
  }
  return actual;
}

void leerPotenciometros() {
  int crudoMax = leerPromedioADC(POT_MAX);
  int crudoMin = leerPromedioADC(POT_MIN);

  if (potMaxFiltrado < 0) { potMaxFiltrado = crudoMax; potMinFiltrado = crudoMin; }
  potMaxFiltrado = 0.8 * potMaxFiltrado + 0.2 * crudoMax;
  potMinFiltrado = 0.8 * potMinFiltrado + 0.2 * crudoMin;

  int nuevoMax = valorConHisteresis(potMaxFiltrado, tempMax, RANGO_MAX_INFERIOR, RANGO_MAX_SUPERIOR, INVERTIR_POT_MAX);
  int nuevoMin = valorConHisteresis(potMinFiltrado, tempMin, RANGO_MIN_INFERIOR, RANGO_MIN_SUPERIOR, INVERTIR_POT_MIN);

  if (nuevoMin > nuevoMax - MARGEN_MINIMO) nuevoMin = nuevoMax - MARGEN_MINIMO;

  if (nuevoMax != tempMax || nuevoMin != tempMin) {
    tempMax = nuevoMax;
    tempMin = nuevoMin;
    Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");
  }
}

void leerSensor() {
  int milivoltios = analogReadMilliVolts(PIN_LM35);

  if (milivoltios > MV_MAXIMO_VALIDO) {
    erroresSensor++;
    filtroListo = false;
    Serial.println("Error: revisar LM35");
    return;
  }

  float lectura = milivoltios / 10.0;

  if (!filtroListo) {
    for (int i = 0; i < MUESTRAS_FILTRO; i++) muestras[i] = lectura;
    filtroListo = true;
  } else {
    muestras[indiceMuestra] = lectura;
  }
  indiceMuestra = (indiceMuestra + 1) % MUESTRAS_FILTRO;
  float suma = 0;
  for (int i = 0; i < MUESTRAS_FILTRO; i++) suma += muestras[i];
  float t = suma / MUESTRAS_FILTRO;

  erroresSensor = 0;
  hayLectura = true;
  tempActual = t;

  porcentaje = (tempActual - tempMin) * 100.0 / (tempMax - tempMin);
  porcentaje = constrain(porcentaje, 0.0, 100.0);

  Serial.print("LM35: ");        Serial.print(lectura, 1);
  Serial.print(" C | Filtrada: "); Serial.print(tempActual, 1);
  Serial.print(" C | ");         Serial.print(porcentaje, 0);
  Serial.print(" % | Estado: "); Serial.println(nombreEstado());
}

void actualizarCiclo() {
  if (erroresSensor >= MAX_ERRORES_SENSOR) {
    if (estado != ERROR_SENSOR) cambiarEstado(ERROR_SENSOR);
    apagarFocoYPeltier();
    actualizarExtractor();
    return;
  }

  if (!hayLectura) return;

  switch (estado) {
    case INICIANDO:
    case ERROR_SENSOR:
      if (erroresSensor == 0) {
        cambiarEstado(tempActual >= tempMax ? ENFRIANDO : CALENTANDO);
      }
      break;

    case CALENTANDO:
      if (tempActual >= tempMax) cambiarEstado(ENFRIANDO);
      break;

    case ENFRIANDO:
      if (tempActual <= tempMin) cambiarEstado(CALENTANDO);
      break;
  }

  aplicarSalidas();
}

void cambiarEstado(EstadoCiclo nuevo) {
  estado = nuevo;
  momentoCambioEstado = millis();
  apagarFocoYPeltier();
  actualizarExtractor();
  Serial.print(">>> Cambio de estado: "); Serial.println(nombreEstado());
}

void actualizarExtractor() {
  bool encender = (estado == ENFRIANDO || estado == ERROR_SENSOR);
  digitalWrite(RELAY_EXTRACTOR, encender ? RELAY_ACTIVO : RELAY_INACTIVO);
}

void aplicarSalidas() {
  actualizarExtractor();

  if (millis() - momentoCambioEstado < PAUSA_CAMBIO_MS) {
    apagarFocoYPeltier();
    return;
  }

  if (estado == CALENTANDO) {
    digitalWrite(RELAY_PELTIER, RELAY_INACTIVO);
    digitalWrite(RELAY_FOCO,    RELAY_ACTIVO);
  } else if (estado == ENFRIANDO) {
    digitalWrite(RELAY_FOCO,    RELAY_INACTIVO);
    digitalWrite(RELAY_PELTIER, RELAY_ACTIVO);
  } else {
    apagarFocoYPeltier();
  }
}

void apagarFocoYPeltier() {
  digitalWrite(RELAY_FOCO,    RELAY_INACTIVO);
  digitalWrite(RELAY_PELTIER, RELAY_INACTIVO);
}

const char* nombreEstado() {
  switch (estado) {
    case CALENTANDO:   return "CALENTANDO";
    case ENFRIANDO:    return "ENFRIANDO";
    case ERROR_SENSOR: return "ERROR SENSOR";
    default:           return "INICIANDO";
  }
}

void actualizarLCD() {
  char linea[17];

  snprintf(linea, sizeof(linea), "Max:%2d%c Min:%2d%c", tempMax, GRADO, tempMin, GRADO);
  imprimirLinea(0, linea);

  if (estado == ERROR_SENSOR) {
    imprimirLinea(1, "Error LM35");
  } else if (!hayLectura) {
    imprimirLinea(1, "Leyendo LM35");
  } else {
    const char* textoEstado;
    if (estado == CALENTANDO)     textoEstado = "Calor";
    else if (estado == ENFRIANDO) textoEstado = "Frio";
    else                          textoEstado = "";

    snprintf(linea, sizeof(linea), "%4.1f%c %3d%% %s",
             tempActual, GRADO, (int)(porcentaje + 0.5), textoEstado);
    imprimirLinea(1, linea);
  }
}

void imprimirLinea(int fila, const char* texto) {
  lcd.setCursor(0, fila);
  int n = 0;
  while (texto[n] != '\0' && n < 16) {
    lcd.print(texto[n]);
    n++;
  }
  while (n < 16) {
    lcd.print(' ');
    n++;
  }
}
