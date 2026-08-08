/* =====================================================================
 *  PAINEL LED LAMBDA  -  Arduino Micro (ATmega32U4) + WS2812B 8x8
 *  Placa de LED: CJMCU-64 (matriz 8x8 = 64 LEDs enderecaveis)
 * ---------------------------------------------------------------------
 *  - Le a tensao da sonda lambda em A0
 *  - Converte tensao -> lambda por reta de calibracao (2 pontos)
 *  - Calcula media movel (janela em ms, taxa de amostragem em Hz)
 *  - Desenha uma barra de colunas coloridas (nivel de lambda)
 *  - A faixa de LINHAS acesas indica a tendencia (subindo/descendo/estavel)
 *  - Alerta (estatico ou piscando) acende os LEDs que estao apagados
 *  - Toda a configuracao vem do app Python via Serial e fica na EEPROM
 * =====================================================================
 */

#include <Arduino.h>
#include <FastLED.h>
#include <EEPROM.h>

// ---------------------------------------------------------------------
//  HARDWARE
// ---------------------------------------------------------------------
#define LED_PIN      6        // Pino de DADOS (DIN) do painel WS2812B
#define LAMBDA_PIN   A0       // Entrada analogica da sonda lambda (0-5V)
#define NUM_LEDS     64
#define MATRIX_W     8
#define MATRIX_H     8
#define SERPENTINE   true     // CJMCU-64 costuma ser "zig-zag" (serpentina)
#define ADC_VREF     5.0f     // Tensao de referencia do ADC (5V no Micro)

CRGB leds[NUM_LEDS];

// ---------------------------------------------------------------------
//  CONFIGURACAO PERSISTENTE (EEPROM)
// ---------------------------------------------------------------------
#define CFG_MAGIC    0xA7
#define CFG_VERSION  1
#define EEPROM_ADDR  0
#define MAX_WINDOW   200       // maximo de amostras da media movel

struct Config {
  uint8_t  magic;
  uint8_t  version;

  // Cor de cada uma das 8 colunas
  uint8_t  colR[8];
  uint8_t  colG[8];
  uint8_t  colB[8];

  // Valor de lambda associado a cada coluna (barra sobe conforme lambda)
  float    colLambda[8];

  // Alerta
  uint16_t alertTime;   // periodo do pisca / tempo do alerta (ms)
  uint8_t  alertType;   // 0 = estatico, 1 = piscando
  uint8_t  alertR, alertG, alertB;

  // Calibracao linear tensao<->lambda (2 pontos)
  float    v1, l1;      // ex.: 1.0V -> 0.40 lambda
  float    v2, l2;      // ex.: 4.0V -> 1.58 lambda

  // Linhas (bitmask 8 bits) acesas por tendencia
  uint8_t  rowsRising;  // rampa subindo
  uint8_t  rowsFalling; // rampa descendo
  uint8_t  rowsStable;  // estavel

  float    stableThresh;// variacao minima p/ considerar estavel (lambda)
  uint16_t mavgTime;    // tempo da media movel (ms)
  uint16_t sampleRate;  // taxa de amostragem (Hz)

  float    alarmLambda; // lambda que dispara o alerta
  uint8_t  brightness;  // brilho global (0-255) - protege a fonte
};

Config cfg;

// ---------------------------------------------------------------------
//  ESTADO EM TEMPO REAL
// ---------------------------------------------------------------------
float   ring[MAX_WINDOW];      // buffer circular de amostras (lambda)
uint8_t ringHead   = 0;
uint16_t ringCount = 0;
uint16_t windowN   = 1;        // nro de amostras da janela atual
float   ringSum    = 0.0f;

float   lastVoltage = 0.0f;
float   lastLambda  = 0.0f;
float   mavg        = 0.0f;

int8_t  trend       = 0;       // -1 desce, 0 estavel, 1 sobe
float   trendRef    = 0.0f;

bool    streaming   = true;

uint32_t sampleInterval = 100; // ms entre amostras (=1000/sampleRate)
uint32_t tSample = 0, tFrame = 0, tTelem = 0, tTrend = 0;

const uint16_t FRAME_MS = 30;   // atualizacao do painel
const uint16_t TELEM_MS = 100;  // envio de telemetria ao app
const uint16_t TREND_MS = 250;  // janela de avaliacao da tendencia

// ---------------------------------------------------------------------
//  DEFAULTS
// ---------------------------------------------------------------------
void loadDefaults() {
  cfg.magic   = CFG_MAGIC;
  cfg.version = CFG_VERSION;

  // Verde -> amarelo -> vermelho ao longo das 8 colunas
  const uint8_t defR[8] = {  0,  0,  0,255,255,255,255,255};
  const uint8_t defG[8] = {255,255,255,255,200,120, 40,  0};
  const uint8_t defB[8] = {  0,  0,  0,  0,  0,  0,  0,  0};
  for (uint8_t i = 0; i < 8; i++) {
    cfg.colR[i] = defR[i];
    cfg.colG[i] = defG[i];
    cfg.colB[i] = defB[i];
    // lambda crescente por coluna (exemplo estequiometrico ~0.7..1.4)
    cfg.colLambda[i] = 0.70f + i * 0.10f;
  }

  cfg.alertTime = 300;
  cfg.alertType = 1;
  cfg.alertR = 255; cfg.alertG = 0; cfg.alertB = 0;

  cfg.v1 = 1.0f; cfg.l1 = 0.40f;
  cfg.v2 = 4.0f; cfg.l2 = 1.58f;

  cfg.rowsRising  = 0b00000111; // 3 linhas de cima
  cfg.rowsFalling = 0b11100000; // 3 linhas de baixo
  cfg.rowsStable  = 0b00111000; // 3 linhas do meio

  cfg.stableThresh = 0.02f;
  cfg.mavgTime     = 500;
  cfg.sampleRate   = 50;
  cfg.alarmLambda  = 1.40f;
  cfg.brightness   = 40;
}

void saveEEPROM() { EEPROM.put(EEPROM_ADDR, cfg); }

void loadEEPROM() {
  EEPROM.get(EEPROM_ADDR, cfg);
  if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION) {
    loadDefaults();
    saveEEPROM();
  }
}

// Recalcula parametros derivados quando a config muda
void applyConfig() {
  if (cfg.sampleRate < 1)   cfg.sampleRate = 1;
  if (cfg.sampleRate > 500) cfg.sampleRate = 500;
  sampleInterval = 1000UL / cfg.sampleRate;
  if (sampleInterval < 2) sampleInterval = 2;

  windowN = (uint16_t)((uint32_t)cfg.mavgTime / sampleInterval);
  if (windowN < 1) windowN = 1;
  if (windowN > MAX_WINDOW) windowN = MAX_WINDOW;

  // zera o buffer da media movel
  ringHead = 0; ringCount = 0; ringSum = 0.0f;
  for (uint16_t i = 0; i < MAX_WINDOW; i++) ring[i] = 0.0f;

  FastLED.setBrightness(cfg.brightness);
}

// ---------------------------------------------------------------------
//  LEITURA / CONVERSAO
// ---------------------------------------------------------------------
float readVoltage() {
  int raw = analogRead(LAMBDA_PIN);
  return (raw * ADC_VREF) / 1023.0f;
}

float toLambda(float v) {
  float dv = cfg.v2 - cfg.v1;
  if (fabs(dv) < 1e-6f) return cfg.l1;
  return cfg.l1 + (v - cfg.v1) * (cfg.l2 - cfg.l1) / dv;
}

void pushSample(float lam) {
  if (ringCount >= windowN) {
    ringSum -= ring[ringHead];   // remove a amostra mais antiga
  } else {
    ringCount++;
  }
  ring[ringHead] = lam;
  ringSum += lam;
  ringHead = (ringHead + 1) % windowN;
  mavg = ringSum / ringCount;
}

// ---------------------------------------------------------------------
//  MAPEAMENTO DA MATRIZ  (coluna x, linha y) -> indice do LED
// ---------------------------------------------------------------------
uint16_t XY(uint8_t x, uint8_t y) {
  uint16_t i;
  if (SERPENTINE && (y & 0x01)) {
    i = (uint16_t)y * MATRIX_W + (MATRIX_W - 1 - x); // linha impar invertida
  } else {
    i = (uint16_t)y * MATRIX_W + x;
  }
  return i;
}

// ---------------------------------------------------------------------
//  RENDER
// ---------------------------------------------------------------------
void render() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  bool on[NUM_LEDS];
  for (uint16_t i = 0; i < NUM_LEDS; i++) on[i] = false;

  // Faixa de linhas conforme a tendencia
  uint8_t rowsMask = (trend > 0) ? cfg.rowsRising
                   : (trend < 0) ? cfg.rowsFalling
                                 : cfg.rowsStable;

  // Barra: acende a coluna se o lambda atual atingiu o valor dela
  for (uint8_t x = 0; x < 8; x++) {
    if (mavg >= cfg.colLambda[x]) {
      CRGB c(cfg.colR[x], cfg.colG[x], cfg.colB[x]);
      for (uint8_t y = 0; y < 8; y++) {
        if (rowsMask & (1 << y)) {
          uint16_t idx = XY(x, y);
          leds[idx] = c;
          on[idx] = true;
        }
      }
    }
  }

  // Alerta: acende/pisca os LEDs que estao apagados
  if (mavg >= cfg.alarmLambda) {
    bool show = true;
    if (cfg.alertType == 1) {               // piscando
      uint16_t p = cfg.alertTime ? cfg.alertTime : 300;
      show = ((millis() / p) & 0x01) == 0;
    }
    if (show) {
      CRGB a(cfg.alertR, cfg.alertG, cfg.alertB);
      for (uint16_t i = 0; i < NUM_LEDS; i++)
        if (!on[i]) leds[i] = a;
    }
  }

  FastLED.show();
}

// ---------------------------------------------------------------------
//  PROTOCOLO SERIAL  (linhas de texto terminadas em \n)
// ---------------------------------------------------------------------
void sendConfig() {
  Serial.print(F("CFG"));
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print(F(" C")); Serial.print(i); Serial.print('=');
    Serial.print(cfg.colR[i]); Serial.print(',');
    Serial.print(cfg.colG[i]); Serial.print(',');
    Serial.print(cfg.colB[i]); Serial.print(',');
    Serial.print(cfg.colLambda[i], 3);
  }
  Serial.print(F(" ALERT=")); Serial.print(cfg.alertTime); Serial.print(',');
  Serial.print(cfg.alertType); Serial.print(',');
  Serial.print(cfg.alertR); Serial.print(','); Serial.print(cfg.alertG);
  Serial.print(','); Serial.print(cfg.alertB);
  Serial.print(F(" CAL=")); Serial.print(cfg.v1, 3); Serial.print(',');
  Serial.print(cfg.l1, 3); Serial.print(','); Serial.print(cfg.v2, 3);
  Serial.print(','); Serial.print(cfg.l2, 3);
  Serial.print(F(" ROWS=")); Serial.print(cfg.rowsRising); Serial.print(',');
  Serial.print(cfg.rowsFalling); Serial.print(','); Serial.print(cfg.rowsStable);
  Serial.print(F(" STAB=")); Serial.print(cfg.stableThresh, 3);
  Serial.print(F(" MAVG=")); Serial.print(cfg.mavgTime);
  Serial.print(F(" SRATE=")); Serial.print(cfg.sampleRate);
  Serial.print(F(" ALARM=")); Serial.print(cfg.alarmLambda, 3);
  Serial.print(F(" BRIGHT=")); Serial.print(cfg.brightness);
  Serial.println();
}

void handleLine(char *line) {
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if      (!strcmp(cmd, "PING"))  { Serial.println(F("PONG PainelLambda v1")); }
  else if (!strcmp(cmd, "GET"))   { sendConfig(); }
  else if (!strcmp(cmd, "SAVE"))  { saveEEPROM(); Serial.println(F("OK SAVE")); }
  else if (!strcmp(cmd, "LOAD"))  { loadEEPROM(); applyConfig(); Serial.println(F("OK LOAD")); }
  else if (!strcmp(cmd, "STREAM")){ char*a=strtok(NULL," "); streaming = a && a[0]=='1'; Serial.println(F("OK STREAM")); }

  else if (!strcmp(cmd, "COLOR")) {              // COLOR i r g b
    int i = atoi(strtok(NULL, " "));
    int r = atoi(strtok(NULL, " "));
    int g = atoi(strtok(NULL, " "));
    int b = atoi(strtok(NULL, " "));
    if (i >= 0 && i < 8) { cfg.colR[i]=r; cfg.colG[i]=g; cfg.colB[i]=b; }
    Serial.println(F("OK COLOR"));
  }
  else if (!strcmp(cmd, "COLLAMBDA")) {          // COLLAMBDA i valor
    int i = atoi(strtok(NULL, " "));
    float v = atof(strtok(NULL, " "));
    if (i >= 0 && i < 8) cfg.colLambda[i] = v;
    Serial.println(F("OK COLLAMBDA"));
  }
  else if (!strcmp(cmd, "ALERT")) {              // ALERT tempo tipo
    cfg.alertTime = atoi(strtok(NULL, " "));
    cfg.alertType = atoi(strtok(NULL, " "));
    Serial.println(F("OK ALERT"));
  }
  else if (!strcmp(cmd, "ALERTCOLOR")) {         // ALERTCOLOR r g b
    cfg.alertR = atoi(strtok(NULL, " "));
    cfg.alertG = atoi(strtok(NULL, " "));
    cfg.alertB = atoi(strtok(NULL, " "));
    Serial.println(F("OK ALERTCOLOR"));
  }
  else if (!strcmp(cmd, "CALIB")) {              // CALIB v1 l1 v2 l2
    cfg.v1 = atof(strtok(NULL, " "));
    cfg.l1 = atof(strtok(NULL, " "));
    cfg.v2 = atof(strtok(NULL, " "));
    cfg.l2 = atof(strtok(NULL, " "));
    Serial.println(F("OK CALIB"));
  }
  else if (!strcmp(cmd, "TRENDROWS")) {          // TRENDROWS rising falling stable
    cfg.rowsRising  = (uint8_t)atoi(strtok(NULL, " "));
    cfg.rowsFalling = (uint8_t)atoi(strtok(NULL, " "));
    cfg.rowsStable  = (uint8_t)atoi(strtok(NULL, " "));
    Serial.println(F("OK TRENDROWS"));
  }
  else if (!strcmp(cmd, "STABLE")) {             // STABLE thresh
    cfg.stableThresh = atof(strtok(NULL, " "));
    Serial.println(F("OK STABLE"));
  }
  else if (!strcmp(cmd, "MAVG")) {               // MAVG ms
    cfg.mavgTime = atoi(strtok(NULL, " "));
    applyConfig();
    Serial.println(F("OK MAVG"));
  }
  else if (!strcmp(cmd, "SRATE")) {              // SRATE hz
    cfg.sampleRate = atoi(strtok(NULL, " "));
    applyConfig();
    Serial.println(F("OK SRATE"));
  }
  else if (!strcmp(cmd, "ALARM")) {              // ALARM lambda
    cfg.alarmLambda = atof(strtok(NULL, " "));
    Serial.println(F("OK ALARM"));
  }
  else if (!strcmp(cmd, "BRIGHT")) {             // BRIGHT 0-255
    cfg.brightness = atoi(strtok(NULL, " "));
    FastLED.setBrightness(cfg.brightness);
    Serial.println(F("OK BRIGHT"));
  }
  else {
    Serial.print(F("ERR ")); Serial.println(cmd);
  }
}

void handleSerial() {
  static char buf[64];
  static uint8_t idx = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (idx > 0) { buf[idx] = 0; handleLine(buf); idx = 0; }
    } else if (idx < sizeof(buf) - 1) {
      buf[idx++] = c;
    }
  }
}

void sendTelemetry() {
  // D <tensao> <lambda> <mavg> <trend> <colunas> <alarme>
  uint8_t cols = 0;
  for (uint8_t x = 0; x < 8; x++) if (mavg >= cfg.colLambda[x]) cols++;
  Serial.print(F("D "));
  Serial.print(lastVoltage, 3); Serial.print(' ');
  Serial.print(lastLambda, 3);  Serial.print(' ');
  Serial.print(mavg, 3);        Serial.print(' ');
  Serial.print(trend);          Serial.print(' ');
  Serial.print(cols);           Serial.print(' ');
  Serial.println(mavg >= cfg.alarmLambda ? 1 : 0);
}

// ---------------------------------------------------------------------
//  SETUP / LOOP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  analogReference(DEFAULT);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  loadEEPROM();
  applyConfig();
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void loop() {
  handleSerial();
  uint32_t now = millis();

  if (now - tSample >= sampleInterval) {
    tSample = now;
    lastVoltage = readVoltage();
    lastLambda  = toLambda(lastVoltage);
    pushSample(lastLambda);
  }

  if (now - tTrend >= TREND_MS) {
    tTrend = now;
    float d = mavg - trendRef;
    if      (d >  cfg.stableThresh) trend =  1;
    else if (d < -cfg.stableThresh) trend = -1;
    else                            trend =  0;
    trendRef = mavg;
  }

  if (now - tFrame >= FRAME_MS) {
    tFrame = now;
    render();
  }

  if (streaming && now - tTelem >= TELEM_MS) {
    tTelem = now;
    sendTelemetry();
  }
}
