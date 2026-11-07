#include <Arduino.h>

// Practica 6: Control ON-OFF de temperatura con teclado matricial 4x4 y LM35 (ESP32)
// A + numero + # = MAX | B + numero + # = MIN | * = borrar

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Preferences.h>

#define PIN_LM35          34   // salida (VOUT) del LM35, alimentado a 5V

#define RELAY_FOCO        26   // foco (calienta)
#define RELAY_PELTIER     27   // celda Peltier + su ventilador (enfria)
#define RELAY_EXTRACTOR   13   // extractor (solo al enfriar)

#define LCD_SDA           21
#define LCD_SCL           22

// Modulo de reles activo en bajo (LOW). Si funcionan al reves, cambia LOW por HIGH.
#define RELAY_ACTIVO   LOW
#define RELAY_INACTIVO (!RELAY_ACTIVO)

const byte FILAS = 4;
const byte COLUMNAS = 4;
char teclas[FILAS][COLUMNAS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte pinesFilas[FILAS]       = { 32, 33, 25, 14 };  // F1, F2, F3, F4
byte pinesColumnas[COLUMNAS] = { 19, 18, 17, 16 };  // C1, C2, C3, C4
Keypad teclado = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, FILAS, COLUMNAS);

const int TEMP_MAX_INICIAL = 60;  // C
const int TEMP_MIN_INICIAL = 30;  // C
const int LIMITE_INFERIOR  = 0;   // C
const int LIMITE_SUPERIOR  = 99;  // C
const int MARGEN_MINIMO    = 5;   // C entre MAX y MIN

const unsigned long PAUSA_CAMBIO_MS = 1000;      // foco y Peltier apagados al cambiar
const int MAX_ERRORES_SENSOR = 3;
const unsigned long TIEMPO_CAPTURA_MS = 10000;
const unsigned long TIEMPO_MENSAJE_MS = 2000;

LiquidCrystal_I2C lcd(0x27, 16, 2);  // si no se ve nada, prueba 0x3F
Preferences prefs;

enum EstadoCiclo { INICIANDO, CALENTANDO, ENFRIANDO, ERROR_SENSOR };
EstadoCiclo estado = INICIANDO;

int tempMax = TEMP_MAX_INICIAL;
int tempMin = TEMP_MIN_INICIAL;

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

enum Pantalla { NORMAL, CAPTURANDO, MENSAJE };
Pantalla pantalla = NORMAL;
bool capturandoMax = true;
char digitos[3] = "";
unsigned long ultimaTecla = 0;
char mensaje1[17] = "";
char mensaje2[17] = "";
unsigned long momentoMensaje = 0;

unsigned long ultimaActualizacionLCD = 0;
const unsigned long INTERVALO_LCD_MS = 300;

const char GRADO = (char)223;

void manejarTeclado();
void iniciarCaptura(bool esMax);
void confirmarCaptura();
void mostrarMensaje(const char* l1, const char* l2);
void validarLimites();
void guardarConfiguracion();
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

  teclado.setDebounceTime(50);

  delay(500);
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LM35, ADC_11db);
  pinMode(PIN_LM35, INPUT);

  prefs.begin("tempTeclado", false);
  tempMax = prefs.getInt("tempMax", TEMP_MAX_INICIAL);
  tempMin = prefs.getInt("tempMin", TEMP_MIN_INICIAL);
  prefs.end();
  validarLimites();

  Serial.println("=== Control de temperatura con teclado ===");
  Serial.println("A + numero + # = MAX | B + numero + # = MIN | * = borrar");
  Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");

  delay(1000);
  lcd.clear();
}

void loop() {
  manejarTeclado();

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

void manejarTeclado() {
  if (pantalla == CAPTURANDO && millis() - ultimaTecla > TIEMPO_CAPTURA_MS) {
    mostrarMensaje("Captura", "cancelada");
  }
  if (pantalla == MENSAJE && millis() - momentoMensaje > TIEMPO_MENSAJE_MS) {
    pantalla = NORMAL;
  }

  char tecla = teclado.getKey();
  if (!tecla) return;

  ultimaTecla = millis();
  Serial.print("Tecla: "); Serial.println(tecla);

  if (tecla == 'A') { iniciarCaptura(true);  return; }
  if (tecla == 'B') { iniciarCaptura(false); return; }

  if (pantalla != CAPTURANDO) return;

  int n = strlen(digitos);
  if (tecla >= '0' && tecla <= '9') {
    if (n < 2) {
      digitos[n] = tecla;
      digitos[n + 1] = '\0';
    }
  } else if (tecla == '*') {
    if (n > 0) digitos[n - 1] = '\0';
    else mostrarMensaje("Captura", "cancelada");
  } else if (tecla == '#') {
    confirmarCaptura();
  }
}

void iniciarCaptura(bool esMax) {
  capturandoMax = esMax;
  digitos[0] = '\0';
  pantalla = CAPTURANDO;
  ultimaTecla = millis();
}

void confirmarCaptura() {
  if (strlen(digitos) == 0) {
    mostrarMensaje("Escribe un", "numero primero");
    return;
  }

  int valor = atoi(digitos);
  int nuevoMax = capturandoMax ? valor : tempMax;
  int nuevoMin = capturandoMax ? tempMin : valor;

  if (valor < LIMITE_INFERIOR || valor > LIMITE_SUPERIOR) {
    mostrarMensaje("Fuera de rango", "Usa 0 a 99");
    return;
  }
  if (nuevoMax < nuevoMin + MARGEN_MINIMO) {
    char l2[24];
    if (capturandoMax) snprintf(l2, sizeof(l2), "MAX >= %d%c", tempMin + MARGEN_MINIMO, GRADO);
    else               snprintf(l2, sizeof(l2), "MIN <= %d%c", tempMax - MARGEN_MINIMO, GRADO);
    mostrarMensaje("Valor no valido", l2);
    return;
  }

  tempMax = nuevoMax;
  tempMin = nuevoMin;
  guardarConfiguracion();

  char l1[24];
  snprintf(l1, sizeof(l1), "%s = %d%c", capturandoMax ? "MAX" : "MIN", valor, GRADO);
  mostrarMensaje(l1, "Guardado");
}

void mostrarMensaje(const char* l1, const char* l2) {
  strncpy(mensaje1, l1, 16); mensaje1[16] = '\0';
  strncpy(mensaje2, l2, 16); mensaje2[16] = '\0';
  pantalla = MENSAJE;
  momentoMensaje = millis();
  Serial.print("LCD: "); Serial.print(mensaje1); Serial.print(" / "); Serial.println(mensaje2);
}

void validarLimites() {
  tempMax = constrain(tempMax, LIMITE_INFERIOR + MARGEN_MINIMO, LIMITE_SUPERIOR);
  tempMin = constrain(tempMin, LIMITE_INFERIOR, tempMax - MARGEN_MINIMO);
}

void guardarConfiguracion() {
  prefs.begin("tempTeclado", false);
  prefs.putInt("tempMax", tempMax);
  prefs.putInt("tempMin", tempMin);
  prefs.end();
  Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");
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

  if (pantalla == CAPTURANDO) {
    snprintf(linea, sizeof(linea), "Nuevo %s: %s%s",
             capturandoMax ? "MAX" : "MIN", digitos, strlen(digitos) < 2 ? "_" : "");
    imprimirLinea(0, linea);
    imprimirLinea(1, "#:OK  *:Borrar");
    return;
  }

  if (pantalla == MENSAJE) {
    imprimirLinea(0, mensaje1);
    imprimirLinea(1, mensaje2);
    return;
  }

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
