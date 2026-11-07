#include <Arduino.h>

// Practica 7: Animatronico con ESP32, sensor PIR, servo, DFPlayer Mini y LEDs
// Al detectar movimiento: suena el audio, se encienden los ojos y la boca se mueve.

#include <ESP32Servo.h>
#include <DFRobotDFPlayerMini.h>

const int PIN_PIR   = 27;
const int PIN_SERVO = 18;
const int PIN_LED1  = 25;
const int PIN_LED2  = 26;
const int PIN_DF_RX = 16;  // recibe del TX del DFPlayer
const int PIN_DF_TX = 17;  // envia al RX del DFPlayer (con resistencia de 1 k)

const int ANGULO_CERRADO = 0;
const int ANGULO_ABIERTO = 50;
const int VELOCIDAD_BOCA = 300;               // ms por movimiento
const unsigned long DURACION_SHOW   = 8000;   // ms
const unsigned long PIR_LOW_ESTABLE = 1000;   // ms con el PIR en LOW para rearmar
const unsigned long ESPERA_MAX_PIR  = 15000;  // ms
const unsigned long CALIBRACION_PIR = 30000;  // ms
const int VOLUMEN = 10;                       // 0 a 30
const int PISTA   = 1;                        // 0001.mp3
const bool LEDS_SIEMPRE_ENCENDIDOS = false;

Servo servoBoca;
DFRobotDFPlayerMini mp3;
bool mp3Listo = false;
bool pirAnterior = false;

void ponerLeds(bool estado) {
  digitalWrite(PIN_LED1, estado);
  digitalWrite(PIN_LED2, estado);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Animatronico Halloween ===");

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  ponerLeds(LEDS_SIEMPRE_ENCENDIDOS);

  servoBoca.setPeriodHertz(50);
  servoBoca.attach(PIN_SERVO, 500, 2400);
  servoBoca.write(ANGULO_CERRADO);

  Serial2.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
  delay(1000);
  if (mp3.begin(Serial2)) {
    mp3Listo = true;
    mp3.volume(VOLUMEN);
    Serial.println("MP3 listo");
  } else {
    Serial.println("No se detecta el MP3: revisa conexiones y tarjeta SD");
  }

  Serial.println("Calibrando sensor PIR (30 s)...");
  unsigned long inicioCal = millis();
  while (millis() - inicioCal < CALIBRACION_PIR) {
    delay(5000);
    Serial.print("  ... ");
    Serial.print((millis() - inicioCal) / 1000);
    Serial.println(" s");
  }

  unsigned long t0 = millis();
  while (digitalRead(PIN_PIR) == HIGH && millis() - t0 < 10000) {
    Serial.println("PIR en HIGH, esperando...");
    delay(500);
  }

  pirAnterior = digitalRead(PIN_PIR);
  Serial.println("Sistema listo");
}

void iniciarShow() {
  Serial.println("Movimiento detectado!");

  if (mp3Listo) mp3.play(PISTA);
  ponerLeds(true);

  unsigned long inicio = millis();
  bool abierta = false;
  while (millis() - inicio < DURACION_SHOW) {
    abierta = !abierta;
    servoBoca.write(abierta ? ANGULO_ABIERTO : ANGULO_CERRADO);
    delay(VELOCIDAD_BOCA);
  }

  servoBoca.write(ANGULO_CERRADO);
  ponerLeds(LEDS_SIEMPRE_ENCENDIDOS);
  if (mp3Listo) mp3.stop();

  // Espera a que el PIR se mantenga en LOW antes de volver a detectar
  Serial.println("Esperando a que el PIR se calme...");
  unsigned long tiempoLow = millis();
  unsigned long tMax = millis();
  while (millis() - tiempoLow < PIR_LOW_ESTABLE && millis() - tMax < ESPERA_MAX_PIR) {
    if (digitalRead(PIN_PIR) == HIGH) tiempoLow = millis();
    delay(20);
  }

  if (digitalRead(PIN_PIR) == HIGH) {
    Serial.println("Aviso: el PIR sigue en HIGH (revisa potenciometro Tx y jumper)");
  }
  Serial.println("Listo para detectar de nuevo");
}

void loop() {
  bool pirActual = digitalRead(PIN_PIR);

  // Solo reacciona al pasar de LOW a HIGH (movimiento nuevo)
  if (pirActual && !pirAnterior) {
    iniciarShow();
    pirAnterior = digitalRead(PIN_PIR);
  } else {
    pirAnterior = pirActual;
  }

  delay(50);
}
