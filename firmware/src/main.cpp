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
#define ALERT_OUT_PIN 10      // Saida do alerta: nivel BAIXO (negativo/GND) quando em alerta
                             // (repouso = HIGH). Use com rele/buzina via transistor.
#define NUM_LEDS     64
#define MATRIX_W     8
#define MATRIX_H     8
#define ADC_VREF     5.0f     // Tensao de referencia do ADC (5V no Micro)
// (o tipo de fiacao - serpentina/espelho/transpor - e configuravel pelo app)

CRGB leds[NUM_LEDS];

// ---------------------------------------------------------------------
//  CONFIGURACAO PERSISTENTE (EEPROM)
// ---------------------------------------------------------------------
#define CFG_MAGIC    0xA7
#define CFG_VERSION  5
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
  uint16_t alertTime;   // periodo do pisca (ms)
  uint8_t  alertType;   // 0 = estatico, 1 = piscando
  uint8_t  alertR, alertG, alertB;
  uint16_t alertHold;   // tempo minimo (ms) que o alerta fica ligado apos disparar
  uint8_t  alertBright; // brilho SOMENTE do alerta (0-255), independente do global

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

  // Motor diesel: a sonda trabalha de valores MAIORES p/ MENORES.
  // Em modo diesel a barra e o alerta acendem quando o valor fica ABAIXO
  // do valor configurado (logica invertida).
  uint8_t  dieselMode;  // 0 = normal (>=)   1 = diesel/invertido (<=)

  // LED verde central: aceso enquanto o valor estiver "em repouso"
  // (acima do primeiro LED a acender / nenhuma coluna ligada).
  uint8_t  centerEnable;
  uint8_t  centerR, centerG, centerB;

  // Mapeamento fisico da matriz (ajuste ate a imagem sair correta):
  uint8_t  mapSerp;      // 1 = fiacao serpentina (zig-zag)  0 = progressiva
  uint8_t  mapFlipX;     // espelha horizontal
  uint8_t  mapFlipY;     // espelha vertical
  uint8_t  mapTranspose; // troca linhas <-> colunas (data entra por outro lado)
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

bool     alarmActive   = false; // alerta efetivo (condicao OU tempo minimo ligado)
uint32_t alertHoldUntil = 0;    // millis() ate quando o alerta deve permanecer ligado

bool    streaming   = true;
bool    testMode    = false;   // padrao de teste p/ ajustar o mapeamento

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
  cfg.alertHold = 3000;   // fica alertando por no minimo 3s apos disparar
  cfg.alertBright = 255;  // alerta no brilho maximo (bem chamativo) por padrao

  cfg.v1 = 1.0f; cfg.l1 = 0.40f;
  cfg.v2 = 4.0f; cfg.l2 = 1.58f;

  cfg.rowsRising  = 0b00000111; // 3 linhas de cima
  cfg.rowsFalling = 0b11100000; // 3 linhas de baixo
  cfg.rowsStable  = 0b00111000; // 3 linhas do meio

  cfg.stableThresh = 0.02f;
  cfg.mavgTime     = 500;
  cfg.sampleRate   = 50;
  cfg.alarmLambda  = 1.33f;
  cfg.brightness   = 40;

  cfg.dieselMode   = 1;            // projeto e para motor diesel
  cfg.centerEnable = 1;
  cfg.centerR = 0; cfg.centerG = 255; cfg.centerB = 0;  // verde

  cfg.mapSerp = 1; cfg.mapFlipX = 0; cfg.mapFlipY = 0; cfg.mapTranspose = 0;
}

// Uma coluna acende conforme o modo (normal x diesel/invertido)
bool isColLit(uint8_t x) {
  return cfg.dieselMode ? (mavg <= cfg.colLambda[x])
                        : (mavg >= cfg.colLambda[x]);
}

// Condicao de alerta conforme o modo
bool isAlarm() {
  return cfg.dieselMode ? (mavg <= cfg.alarmLambda)
                        : (mavg >= cfg.alarmLambda);
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

  // O brilho e aplicado por pixel no render() (barra x alerta tem brilhos
  // separados), entao o brilho global do FastLED fica em 255.
  FastLED.setBrightness(255);
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
  uint8_t X = x, Y = y;
  if (cfg.mapTranspose) { uint8_t t = X; X = Y; Y = t; }
  if (cfg.mapFlipX) X = MATRIX_W - 1 - X;
  if (cfg.mapFlipY) Y = MATRIX_H - 1 - Y;

  uint16_t i;
  if (cfg.mapSerp && (Y & 0x01)) {
    i = (uint16_t)Y * MATRIX_W + (MATRIX_W - 1 - X); // linha impar invertida
  } else {
    i = (uint16_t)Y * MATRIX_W + X;
  }
  return i;
}

// ---------------------------------------------------------------------
//  RENDER
// ---------------------------------------------------------------------
// Padrao de teste: linha de cima VERMELHA, coluna da esquerda AZUL,
// canto (0,0) BRANCO. Se o mapeamento estiver errado a linha/coluna
// aparece em zig-zag ou no lado errado -> ajuste serp/flip/transpose.
void renderTest() {
  digitalWrite(ALERT_OUT_PIN, HIGH);   // saida do alerta em repouso durante o teste
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t x = 0; x < MATRIX_W; x++) leds[XY(x, 0)] = CRGB::Red;   // linha 1 (topo)
  for (uint8_t y = 0; y < MATRIX_H; y++) leds[XY(0, y)] = CRGB::Blue;  // coluna 1 (esq)
  leds[XY(0, 0)] = CRGB::White;                                        // canto sup. esq.
  leds[XY(7, 0)] = CRGB::Green;                                        // canto sup. dir.
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i].nscale8_video(cfg.brightness);
  FastLED.show();
}

void render() {
  if (testMode) { renderTest(); return; }
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  bool on[NUM_LEDS];
  for (uint16_t i = 0; i < NUM_LEDS; i++) on[i] = false;

  // Faixa de linhas conforme a tendencia
  uint8_t rowsMask = (trend > 0) ? cfg.rowsRising
                   : (trend < 0) ? cfg.rowsFalling
                                 : cfg.rowsStable;

  // Barra: acende a coluna conforme o modo (normal x diesel/invertido)
  uint8_t colsLit = 0;
  for (uint8_t x = 0; x < 8; x++) {
    if (isColLit(x)) {
      colsLit++;
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

  // LED verde central (quadrado 2x2): painel em repouso, nenhuma coluna acesa
  // = valor acima do primeiro LED a acender.
  if (cfg.centerEnable && colsLit == 0) {
    CRGB g(cfg.centerR, cfg.centerG, cfg.centerB);
    const uint8_t cx[2] = {3, 4}, cy[2] = {3, 4};
    for (uint8_t a = 0; a < 2; a++)
      for (uint8_t b = 0; b < 2; b++) {
        uint16_t idx = XY(cx[a], cy[b]);
        leds[idx] = g;
        on[idx] = true;
      }
  }

  // Brilho da barra/LED central: aplicado so nos pixels "acesos" (on),
  // com o brilho GLOBAL configurado (que costuma ser baixo p/ proteger a fonte).
  for (uint16_t i = 0; i < NUM_LEDS; i++)
    if (on[i]) leds[i].nscale8_video(cfg.brightness);

  // Alerta: acende/pisca os LEDs apagados e aciona a saida fisica do alerta.
  // Usa alarmActive (condicao + tempo minimo ligado), atualizado no loop().
  // O alerta tem brilho PROPRIO (cfg.alertBright), independente do global.
  bool alertOutActive = false;
  if (alarmActive) {
    bool show = true;
    if (cfg.alertType == 1) {               // piscando
      uint16_t p = cfg.alertTime ? cfg.alertTime : 300;
      show = ((millis() / p) & 0x01) == 0;
    }
    if (show) {
      CRGB a(cfg.alertR, cfg.alertG, cfg.alertB);
      a.nscale8_video(cfg.alertBright);     // brilho proprio do alerta
      for (uint16_t i = 0; i < NUM_LEDS; i++)
        if (!on[i]) leds[i] = a;
    }
    alertOutActive = show;   // acompanha o pisca do painel
  }
  // Saida fisica do alerta: LOW = negativo/GND ativo, HIGH = repouso
  digitalWrite(ALERT_OUT_PIN, alertOutActive ? LOW : HIGH);

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
  Serial.print(','); Serial.print(cfg.alertHold);
  Serial.print(F(" ABRIGHT=")); Serial.print(cfg.alertBright);
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
  Serial.print(F(" DIESEL=")); Serial.print(cfg.dieselMode);
  Serial.print(F(" CENTER=")); Serial.print(cfg.centerEnable); Serial.print(',');
  Serial.print(cfg.centerR); Serial.print(','); Serial.print(cfg.centerG);
  Serial.print(','); Serial.print(cfg.centerB);
  Serial.print(F(" MAP=")); Serial.print(cfg.mapSerp); Serial.print(',');
  Serial.print(cfg.mapFlipX); Serial.print(','); Serial.print(cfg.mapFlipY);
  Serial.print(','); Serial.print(cfg.mapTranspose);
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
  else if (!strcmp(cmd, "ALERT")) {              // ALERT periodo tipo [hold]
    cfg.alertTime = atoi(strtok(NULL, " "));
    cfg.alertType = atoi(strtok(NULL, " "));
    char *h = strtok(NULL, " ");
    if (h) cfg.alertHold = atoi(h);             // opcional (compat. com versoes antigas)
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
  else if (!strcmp(cmd, "BRIGHT")) {             // BRIGHT 0-255 (brilho global da barra)
    cfg.brightness = atoi(strtok(NULL, " "));
    Serial.println(F("OK BRIGHT"));             // aplicado por pixel no render()
  }
  else if (!strcmp(cmd, "ABRIGHT")) {            // ABRIGHT 0-255 (brilho so do alerta)
    cfg.alertBright = atoi(strtok(NULL, " "));
    Serial.println(F("OK ABRIGHT"));
  }
  else if (!strcmp(cmd, "DIESEL")) {             // DIESEL 0|1
    cfg.dieselMode = atoi(strtok(NULL, " ")) ? 1 : 0;
    Serial.println(F("OK DIESEL"));
  }
  else if (!strcmp(cmd, "CENTER")) {             // CENTER enable r g b
    cfg.centerEnable = atoi(strtok(NULL, " ")) ? 1 : 0;
    cfg.centerR = atoi(strtok(NULL, " "));
    cfg.centerG = atoi(strtok(NULL, " "));
    cfg.centerB = atoi(strtok(NULL, " "));
    Serial.println(F("OK CENTER"));
  }
  else if (!strcmp(cmd, "MAP")) {                // MAP serp flipx flipy transpose
    cfg.mapSerp      = atoi(strtok(NULL, " ")) ? 1 : 0;
    cfg.mapFlipX     = atoi(strtok(NULL, " ")) ? 1 : 0;
    cfg.mapFlipY     = atoi(strtok(NULL, " ")) ? 1 : 0;
    cfg.mapTranspose = atoi(strtok(NULL, " ")) ? 1 : 0;
    Serial.println(F("OK MAP"));
  }
  else if (!strcmp(cmd, "TEST")) {               // TEST 0|1
    testMode = atoi(strtok(NULL, " ")) ? true : false;
    Serial.println(F("OK TEST"));
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
  // D <tensao> <lambda> <mavg> <trend> <colMask> <alarme> <central>
  uint8_t colMask = 0;
  for (uint8_t x = 0; x < 8; x++) if (isColLit(x)) colMask |= (1 << x);
  uint8_t center = (cfg.centerEnable && colMask == 0) ? 1 : 0;
  Serial.print(F("D "));
  Serial.print(lastVoltage, 3); Serial.print(' ');
  Serial.print(lastLambda, 3);  Serial.print(' ');
  Serial.print(mavg, 3);        Serial.print(' ');
  Serial.print(trend);          Serial.print(' ');
  Serial.print(colMask);        Serial.print(' ');
  Serial.print(alarmActive ? 1 : 0); Serial.print(' ');
  Serial.println(center);
}

// ---------------------------------------------------------------------
//  SETUP / LOOP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  analogReference(DEFAULT);
  pinMode(ALERT_OUT_PIN, OUTPUT);
  digitalWrite(ALERT_OUT_PIN, HIGH);   // repouso (sem alerta)
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

  // Alerta com tempo minimo ligado (hold): ao disparar, estende o prazo; o alerta
  // so desliga depois que a condicao cessou E o prazo minimo passou.
  bool rawAlarm = isAlarm();
  if (rawAlarm) alertHoldUntil = now + cfg.alertHold;
  alarmActive = rawAlarm || ((int32_t)(alertHoldUntil - now) > 0);

  if (now - tFrame >= FRAME_MS) {
    tFrame = now;
    render();
  }

  if (streaming && now - tTelem >= TELEM_MS) {
    tTelem = now;
    sendTelemetry();
  }
}
