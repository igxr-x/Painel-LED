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

// GPS "pegou sinal": a partir de quantos satelites o anel azul ao redor do
// quadrado central (em repouso) acende, indicando que o fix ja esta vindo.
#define GPS_FIX_SATS 2        // acende com MAIS de 2 satelites (>= 3)

CRGB leds[NUM_LEDS];

// ---------------------------------------------------------------------
//  CONFIGURACAO PERSISTENTE (EEPROM)
// ---------------------------------------------------------------------
#define CFG_MAGIC    0xA7
#define CFG_VERSION  6
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

  // Indicador central "abaixo do limite": mesmo quadrado 2x2 central, mas
  // acende (com cor propria) quando a media movel cai ABAIXO de lowLambda.
  // Espelha o LED central de repouso para o extremo oposto da escala.
  uint8_t  lowEnable;
  float    lowLambda;
  uint8_t  lowR, lowG, lowB;

  // Mapeamento fisico da matriz (ajuste ate a imagem sair correta):
  uint8_t  mapSerp;      // 1 = fiacao serpentina (zig-zag)  0 = progressiva
  uint8_t  mapFlipX;     // espelha horizontal
  uint8_t  mapFlipY;     // espelha vertical
  uint8_t  mapTranspose; // troca linhas <-> colunas (data entra por outro lado)
};

Config cfg;

// ---------------------------------------------------------------------
//  CONFIGURACAO DO GPS / BOLINHAS  (EEPROM SEPARADA - nao mexe na Config)
// ---------------------------------------------------------------------
//  Fica num endereco proprio da EEPROM para que a config principal (cores,
//  calibracao, lambdas, etc.) NAO seja apagada ao adicionar o modulo GPS.
//  Modulo: GPS6MV2 / HW-248 (NEO-6M) ligado na Serial1 do Arduino Micro:
//    TX do GPS -> D0 (RX1 do Micro)   |   VCC 5V   |   GND comum
#define GCFG_MAGIC   0x6D
#define GCFG_VERSION 3
#define GCFG_ADDR    400      // bem depois da Config (que fica em 0)

struct GpsCfg {
  uint8_t  magic;
  uint8_t  version;

  uint8_t  enable;        // 1 = liga a contagem de bolinhas + linha reservada no painel

  // Linha de chegada como PONTO + RAIO. Conta uma volta quando o trecho percorrido
  // (posicao anterior->atual) passa a menos de 'rangeM' metros do ponto. Pratico:
  // capture com o caminhao PARADO no box (de frente p/ a linha) e use raio ~30 m.
  float    latP, lonP;    // ponto de referencia da linha de chegada
  uint16_t rangeM;        // raio de deteccao em metros

  // "Bolinha": estouro do valor de sonda permitido pela prova. Limite SEPARADO
  // do alerta visual. Em modo diesel conta quando a media movel fica <= bolimLambda.
  float    bolimLambda;   // limite de sonda que caracteriza a bolinha
  uint16_t bolimDebounceMs; // tempo de bloqueio (ms) apos contar: so conta outra
                            // bolinha depois desse tempo (regra: 2 s, configuravel)
  uint8_t  bolimLimit;    // a partir deste nro de bolinhas = penalizado (ex.: 6)

  // Linha da borda reservada so pro contador (0 = topo ... 7 = base)
  uint8_t  bolimRow;

  // Cores do contador: normal / atencao (falta 1) / estourado
  uint8_t  nR, nG, nB;    // normal
  uint8_t  wR, wG, wB;    // atencao
  uint8_t  oR, oG, oB;    // estourado

  uint16_t minLapMs;      // tempo minimo entre cruzamentos (anti-duplo, ms)
  uint8_t  minSpeed;      // velocidade minima p/ validar cruzamento (km/h)
};

GpsCfg gcfg;

// ---------------------------------------------------------------------
//  ESTADO DO GPS / BOLINHAS EM TEMPO REAL
// ---------------------------------------------------------------------
bool    gValid   = false;      // fix valido (RMC status = A)
uint8_t gSats    = 0;          // satelites (do GGA)
float   gLat = 0.0f, gLon = 0.0f;
float   gPrevLat = 0.0f, gPrevLon = 0.0f;
bool    gHasPrev = false;
float   gSpeedKmh = 0.0f;
float   gCourse   = 0.0f;
float   gDistM    = -1.0f;      // distancia atual ate o ponto (m); <0 = sem ponto

uint16_t bolinhas        = 0;  // bolinhas da VOLTA atual
uint16_t lastLapBolinhas = 0;  // bolinhas da volta anterior (fechada ao cruzar)
uint16_t lapCount        = 0;  // numero de voltas contadas
bool     bolArmed        = true; // pronto p/ contar (tempo de bloqueio ja passou)
uint32_t lastBolMs       = 0;  // millis() da ultima bolinha contada
uint32_t lastCrossMs     = 0;  // millis() do ultimo cruzamento da linha
uint32_t tGtelem = 0;          // timer da telemetria do GPS

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

  cfg.lowEnable = 0;
  cfg.lowLambda = 0.80f;
  cfg.lowR = 0; cfg.lowG = 0; cfg.lowB = 255;           // azul (distinto do verde)

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

// ---- GPS / bolinhas: defaults e EEPROM propria ----------------------
void loadGpsDefaults() {
  gcfg.magic   = GCFG_MAGIC;
  gcfg.version = GCFG_VERSION;
  gcfg.enable  = 0;                 // desligado ate configurar o ponto da linha
  gcfg.latP = 0.0f; gcfg.lonP = 0.0f;
  gcfg.rangeM = 30;                 // raio de deteccao padrao: 30 m
  gcfg.bolimLambda = 1.33f;         // mesmo default do alarme, mas independente
  gcfg.bolimDebounceMs = 2000;      // regra: 2 s de bloqueio entre bolinhas
  gcfg.bolimLimit  = 6;             // 6 ou mais = penalizado
  gcfg.bolimRow    = 7;             // linha de baixo reservada pro contador
  gcfg.nR = 0;   gcfg.nG = 255; gcfg.nB = 0;    // normal  = verde
  gcfg.wR = 255; gcfg.wG = 140; gcfg.wB = 0;    // atencao = ambar
  gcfg.oR = 255; gcfg.oG = 0;   gcfg.oB = 0;    // estouro = vermelho
  gcfg.minLapMs = 15000;           // no minimo 15 s entre voltas
  gcfg.minSpeed = 20;              // so conta cruzamento acima de 20 km/h
}

void saveGps() { EEPROM.put(GCFG_ADDR, gcfg); }

void loadGps() {
  EEPROM.get(GCFG_ADDR, gcfg);
  if (gcfg.magic != GCFG_MAGIC || gcfg.version != GCFG_VERSION) {
    loadGpsDefaults();
    saveGps();
  }
}

// O ponto da linha de chegada so vale se foi definido (nao-zero)
bool pointValid() {
  return (gcfg.latP != 0.0f || gcfg.lonP != 0.0f);
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
//  GPS  (NEO-6M / GPS6MV2-HW248 na Serial1, NMEA 9600 bps)
// ---------------------------------------------------------------------
// Converte "ddmm.mmmm" (NMEA) -> graus decimais. Serve p/ lat e lon: os
// minutos sao sempre os 2 digitos antes do ponto decimal.
float nmeaToDeg(const char *f, char hemi) {
  if (!f || !*f) return 0.0f;
  double v   = atof(f);
  int    deg = (int)(v / 100.0);
  double min = v - deg * 100.0;
  double d   = deg + min / 60.0;
  if (hemi == 'S' || hemi == 's' || hemi == 'W' || hemi == 'w') d = -d;
  return (float)d;
}

// Distancia (m) do PONTO de referencia (plat,plon) ao SEGMENTO percorrido
// (lat1,lon1)->(lat2,lon2). Usa aproximacao planar local (equirretangular),
// otima na escala de uma pista. Assim nao "pula" o ponto entre duas leituras.
float distSegPointM(float lat1, float lon1, float lat2, float lon2,
                    float plat, float plon) {
  const float M_LAT = 111320.0f;
  float m_lon = 111320.0f * cos(plat * 0.01745329f);   // graus->rad
  // coordenadas em metros relativas ao ponto (origem)
  float ax = (lon1 - plon) * m_lon, ay = (lat1 - plat) * M_LAT;
  float bx = (lon2 - plon) * m_lon, by = (lat2 - plat) * M_LAT;
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  float t = (len2 > 0.0f) ? -(ax * dx + ay * dy) / len2 : 0.0f;
  if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
  float cx = ax + t * dx, cy = ay + t * dy;   // ponto mais proximo no segmento
  return sqrt(cx * cx + cy * cy);
}

// Distancia (m) de uma posicao ao ponto de referencia (p/ telemetria).
float distToPointM(float lat, float lon) {
  const float M_LAT = 111320.0f;
  float m_lon = 111320.0f * cos(gcfg.latP * 0.01745329f);
  float dx = (lon - gcfg.lonP) * m_lon, dy = (lat - gcfg.latP) * M_LAT;
  return sqrt(dx * dx + dy * dy);
}

// Detecta a passagem pela linha de chegada e fecha a volta (zera as bolinhas).
void checkLapCross() {
  if (!pointValid()) { gDistM = -1.0f; gHasPrev = false; return; }
  gDistM = distToPointM(gLat, gLon);
  if (gcfg.enable && gHasPrev && gSpeedKmh >= gcfg.minSpeed) {
    float d = distSegPointM(gPrevLat, gPrevLon, gLat, gLon, gcfg.latP, gcfg.lonP);
    if (d <= gcfg.rangeM) {
      uint32_t now = millis();
      if (now - lastCrossMs > gcfg.minLapMs) {
        lastCrossMs     = now;
        lastLapBolinhas = bolinhas;   // guarda o resultado da volta que fechou
        bolinhas        = 0;          // zera p/ a nova volta
        bolArmed        = true;
        lapCount++;
      }
    }
  }
  gPrevLat = gLat; gPrevLon = gLon; gHasPrev = true;
}

// Quebra a sentenca em campos por virgula PRESERVANDO campos vazios (strtok
// colapsaria ",," e desalinharia os indices). Corta tambem no '*' do checksum.
// Retorna a quantidade de campos; preenche fld[] com ponteiros (in-place).
uint8_t splitNMEA(char *s, char **fld, uint8_t maxf) {
  uint8_t n = 0;
  if (maxf == 0) return 0;
  fld[n++] = s;
  for (char *p = s; *p; p++) {
    if (*p == ',') {
      *p = 0;
      if (n < maxf) fld[n++] = p + 1; else break;
    } else if (*p == '*') {           // fim dos dados (comeca o checksum)
      *p = 0;
      break;
    }
  }
  return n;
}

// Interpreta uma sentenca NMEA ja completa (sem o \r\n). Le RMC (posicao,
// validade, velocidade, rumo) e GGA (satelites). Aceita GP/GN (GNSS misto).
void parseNMEA(char *s) {
  if (s[0] != '$' || strlen(s) < 7) return;
  char *fld[16];
  uint8_t nf = splitNMEA(s, fld, 16);
  if (nf < 1 || strlen(fld[0]) < 6) return;
  char *type = fld[0] + 3;              // pula "$GP"/"$GN"/"$GL"...

  if (!strncmp(type, "RMC", 3) && nf >= 9) {
    // $..RMC,time,status,lat,NS,lon,EW,speed,course,date,...
    gValid = (fld[2][0] == 'A');
    if (gValid) {
      gLat = nmeaToDeg(fld[3], fld[4][0] ? fld[4][0] : 'N');
      gLon = nmeaToDeg(fld[5], fld[6][0] ? fld[6][0] : 'E');
      if (fld[7][0]) gSpeedKmh = atof(fld[7]) * 1.852f;   // nos -> km/h
      if (fld[8][0]) gCourse   = atof(fld[8]);
      checkLapCross();
    }
  }
  else if (!strncmp(type, "GGA", 3) && nf >= 8) {
    // $..GGA,time,lat,NS,lon,EW,fixqual,numsats,...
    gSats = (uint8_t)atoi(fld[7]);
  }
}

// Le a Serial1 do GPS, montando uma sentenca por vez.
void readGps() {
  static char line[84];
  static uint8_t idx = 0;
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (idx > 0) { line[idx] = 0; parseNMEA(line); idx = 0; }
    } else if (idx < sizeof(line) - 1) {
      line[idx++] = c;
    } else {
      idx = 0;   // sentenca gigante/corrompida: descarta
    }
  }
}

// Atualiza a contagem de bolinhas (estouro do valor de sonda permitido).
// Regra: ao contar uma bolinha, ha um TEMPO DE BLOQUEIO (bolimDebounceMs, ex.: 2 s)
// antes de poder contar outra. Se a sonda continuar/voltar a estourar depois desse
// tempo, conta de novo. Isso evita varias bolinhas seguidas pela mesma oscilacao.
void updateBolinha() {
  if (!gcfg.enable) return;
  uint32_t now = millis();
  bool viol = cfg.dieselMode ? (mavg <= gcfg.bolimLambda)
                             : (mavg >= gcfg.bolimLambda);
  if (!bolArmed && (now - lastBolMs >= gcfg.bolimDebounceMs)) bolArmed = true;
  if (viol && bolArmed) {
    bolinhas++;
    bolArmed  = false;
    lastBolMs = now;
  }
}

// Desenha a linha da borda reservada ao contador de bolinhas (por cima de tudo).
uint16_t XY(uint8_t x, uint8_t y);   // definida na secao de MAPEAMENTO
void drawBolinhaRow() {
  if (!gcfg.enable) return;
  uint8_t row = gcfg.bolimRow & 0x07;
  uint8_t lim = gcfg.bolimLimit < 2 ? 2 : gcfg.bolimLimit;
  bool over = bolinhas >= lim;
  bool warn = bolinhas == (uint16_t)(lim - 1);

  CRGB c = over ? CRGB(gcfg.oR, gcfg.oG, gcfg.oB)
         : warn ? CRGB(gcfg.wR, gcfg.wG, gcfg.wB)
                : CRGB(gcfg.nR, gcfg.nG, gcfg.nB);

  // limpa a linha reservada
  for (uint8_t x = 0; x < 8; x++) leds[XY(x, row)] = CRGB::Black;

  // estourado pisca a linha inteira; senao acende 1 LED por bolinha
  bool show = true;
  if (over) {
    uint16_t p = cfg.alertTime ? cfg.alertTime : 300;
    show = ((millis() / p) & 0x01) == 0;
  }
  if (show) {
    uint8_t n = over ? 8 : (bolinhas > 8 ? 8 : (uint8_t)bolinhas);
    for (uint8_t x = 0; x < n; x++) {
      CRGB cc = c;
      cc.nscale8_video(cfg.brightness);
      leds[XY(x, row)] = cc;
    }
  }
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

  // Indicador central "abaixo do limite": quando a media movel cai ABAIXO de
  // lowLambda, o painel mostra so o quadrado 2x2 central (na cor low), igual ao
  // LED de repouso, mas para o extremo oposto. Tem prioridade sobre a barra.
  bool lowActive = cfg.lowEnable && (mavg <= cfg.lowLambda);

  if (lowActive) {
    CRGB lc(cfg.lowR, cfg.lowG, cfg.lowB);
    const uint8_t cx[2] = {3, 4}, cy[2] = {3, 4};
    for (uint8_t a = 0; a < 2; a++)
      for (uint8_t b = 0; b < 2; b++) {
        uint16_t idx = XY(cx[a], cy[b]);
        leds[idx] = lc;
        on[idx] = true;
      }
  } else {
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

      // Indicador "GPS pegou sinal": so enquanto o quadrado central esta aceso
      // (painel em repouso). Com mais de GPS_FIX_SATS satelites, acende o anel
      // 4x4 azul ao redor do centro (12 LEDs). Ao sair do centro (barra sobe),
      // volta ao funcionamento normal. Roda independente do PC (gSats vem do GGA).
      if (gSats > GPS_FIX_SATS) {
        CRGB bl(0, 0, 255);                       // azul (sinal OK)
        for (uint8_t x = 2; x <= 5; x++)
          for (uint8_t y = 2; y <= 5; y++) {
            if (x == 2 || x == 5 || y == 2 || y == 5) {   // so a borda do 4x4
              uint16_t idx = XY(x, y);
              leds[idx] = bl;
              on[idx] = true;
            }
          }
      }
    }
  }

  // Brilho da barra/LED central: aplicado so nos pixels "acesos" (on),
  // com o brilho GLOBAL configurado (que costuma ser baixo p/ proteger a fonte).
  for (uint16_t i = 0; i < NUM_LEDS; i++)
    if (on[i]) leds[i].nscale8_video(cfg.brightness);

  // Alerta: acende/pisca os LEDs apagados e aciona a saida fisica do alerta.
  // Usa alarmActive (condicao + tempo minimo ligado), atualizado no loop().
  // O alerta tem brilho PROPRIO (cfg.alertBright), independente do global.
  // O indicador "abaixo do limite" tem PRIORIDADE: enquanto ativo, o alerta fica
  // suprimido (nada pisca e a saida fisica fica em repouso) -> painel "calmo".
  bool alertOutActive = false;
  if (alarmActive && !lowActive) {
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

  // Linha reservada ao contador de bolinhas: desenhada POR CIMA de tudo
  // (barra, alerta, central), so quando o modo GPS/bolinhas esta ligado.
  drawBolinhaRow();

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
  Serial.print(F(" LOW=")); Serial.print(cfg.lowEnable); Serial.print(',');
  Serial.print(cfg.lowLambda, 3); Serial.print(',');
  Serial.print(cfg.lowR); Serial.print(','); Serial.print(cfg.lowG);
  Serial.print(','); Serial.print(cfg.lowB);
  Serial.print(F(" MAP=")); Serial.print(cfg.mapSerp); Serial.print(',');
  Serial.print(cfg.mapFlipX); Serial.print(','); Serial.print(cfg.mapFlipY);
  Serial.print(','); Serial.print(cfg.mapTranspose);
  Serial.println();
}

// Dump da config do GPS/bolinhas (o app le p/ preencher a aba Prova)
void sendGpsConfig() {
  Serial.print(F("GCFG"));
  Serial.print(F(" EN="));   Serial.print(gcfg.enable);
  Serial.print(F(" PT="));   Serial.print(gcfg.latP, 6); Serial.print(',');
  Serial.print(gcfg.lonP, 6);
  Serial.print(F(" RANGE=")); Serial.print(gcfg.rangeM);
  Serial.print(F(" BOLIM=")); Serial.print(gcfg.bolimLambda, 3); Serial.print(',');
  Serial.print(gcfg.bolimLimit);
  Serial.print(F(" DEB=")); Serial.print(gcfg.bolimDebounceMs);
  Serial.print(F(" ROW="));  Serial.print(gcfg.bolimRow);
  Serial.print(F(" BC0=")); Serial.print(gcfg.nR); Serial.print(',');
  Serial.print(gcfg.nG); Serial.print(','); Serial.print(gcfg.nB);
  Serial.print(F(" BC1=")); Serial.print(gcfg.wR); Serial.print(',');
  Serial.print(gcfg.wG); Serial.print(','); Serial.print(gcfg.wB);
  Serial.print(F(" BC2=")); Serial.print(gcfg.oR); Serial.print(',');
  Serial.print(gcfg.oG); Serial.print(','); Serial.print(gcfg.oB);
  Serial.print(F(" LAP=")); Serial.print(gcfg.minLapMs); Serial.print(',');
  Serial.print(gcfg.minSpeed);
  Serial.println();
}

void handleLine(char *line) {
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if      (!strcmp(cmd, "PING"))  { Serial.println(F("PONG PainelLambda v1")); }
  else if (!strcmp(cmd, "GET"))   { sendConfig(); sendGpsConfig(); }
  else if (!strcmp(cmd, "GGET"))  { sendGpsConfig(); }
  else if (!strcmp(cmd, "SAVE"))  { saveEEPROM(); saveGps(); Serial.println(F("OK SAVE")); }
  else if (!strcmp(cmd, "LOAD"))  { loadEEPROM(); applyConfig(); loadGps(); Serial.println(F("OK LOAD")); }
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
  else if (!strcmp(cmd, "LOW")) {                // LOW enable lambda r g b
    cfg.lowEnable = atoi(strtok(NULL, " ")) ? 1 : 0;
    cfg.lowLambda = atof(strtok(NULL, " "));
    cfg.lowR = atoi(strtok(NULL, " "));
    cfg.lowG = atoi(strtok(NULL, " "));
    cfg.lowB = atoi(strtok(NULL, " "));
    Serial.println(F("OK LOW"));
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
  // ---- GPS / bolinhas ----
  else if (!strcmp(cmd, "GPSEN")) {              // GPSEN 0|1
    gcfg.enable = atoi(strtok(NULL, " ")) ? 1 : 0;
    Serial.println(F("OK GPSEN"));
  }
  else if (!strcmp(cmd, "GPSPT")) {              // GPSPT lat lon  (ponto da linha)
    gcfg.latP = atof(strtok(NULL, " "));
    gcfg.lonP = atof(strtok(NULL, " "));
    gHasPrev = false;                            // reinicia o rastro apos remarcar
    Serial.println(F("OK GPSPT"));
  }
  else if (!strcmp(cmd, "GRANGE")) {             // GRANGE metros  (raio de deteccao)
    gcfg.rangeM = (uint16_t)atol(strtok(NULL, " "));
    Serial.println(F("OK GRANGE"));
  }
  else if (!strcmp(cmd, "BOLIM")) {              // BOLIM lambda limite
    gcfg.bolimLambda = atof(strtok(NULL, " "));
    char *l = strtok(NULL, " ");
    if (l) gcfg.bolimLimit = (uint8_t)atoi(l);
    Serial.println(F("OK BOLIM"));
  }
  else if (!strcmp(cmd, "BDEB")) {               // BDEB ms  (tempo de bloqueio entre bolinhas)
    gcfg.bolimDebounceMs = (uint16_t)atol(strtok(NULL, " "));
    Serial.println(F("OK BDEB"));
  }
  else if (!strcmp(cmd, "BROW")) {               // BROW linha(0..7)
    gcfg.bolimRow = (uint8_t)atoi(strtok(NULL, " ")) & 0x07;
    Serial.println(F("OK BROW"));
  }
  else if (!strcmp(cmd, "BCOL")) {               // BCOL idx r g b  (0=normal 1=atencao 2=estouro)
    int idx = atoi(strtok(NULL, " "));
    int r = atoi(strtok(NULL, " "));
    int g = atoi(strtok(NULL, " "));
    int b = atoi(strtok(NULL, " "));
    if      (idx == 0) { gcfg.nR = r; gcfg.nG = g; gcfg.nB = b; }
    else if (idx == 1) { gcfg.wR = r; gcfg.wG = g; gcfg.wB = b; }
    else if (idx == 2) { gcfg.oR = r; gcfg.oG = g; gcfg.oB = b; }
    Serial.println(F("OK BCOL"));
  }
  else if (!strcmp(cmd, "BLAP")) {               // BLAP minLapMs minSpeedKmh
    gcfg.minLapMs = (uint16_t)atoi(strtok(NULL, " "));
    char *s = strtok(NULL, " ");
    if (s) gcfg.minSpeed = (uint8_t)atoi(s);
    Serial.println(F("OK BLAP"));
  }
  else if (!strcmp(cmd, "BRESET")) {             // zera as bolinhas da volta atual
    bolinhas = 0; bolArmed = true;
    Serial.println(F("OK BRESET"));
  }
  else if (!strcmp(cmd, "LAPRESET")) {           // zera bolinhas + contador de voltas
    bolinhas = 0; lastLapBolinhas = 0; lapCount = 0; bolArmed = true; gHasPrev = false;
    Serial.println(F("OK LAPRESET"));
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
  // D <tensao> <lambda> <mavg> <trend> <colMask> <alarme> <central> <baixo>
  uint8_t colMask = 0;
  for (uint8_t x = 0; x < 8; x++) if (isColLit(x)) colMask |= (1 << x);
  uint8_t low = (cfg.lowEnable && (mavg <= cfg.lowLambda)) ? 1 : 0;
  uint8_t center = (cfg.centerEnable && colMask == 0) ? 1 : 0;
  Serial.print(F("D "));
  Serial.print(lastVoltage, 3); Serial.print(' ');
  Serial.print(lastLambda, 3);  Serial.print(' ');
  Serial.print(mavg, 3);        Serial.print(' ');
  Serial.print(trend);          Serial.print(' ');
  Serial.print(colMask);        Serial.print(' ');
  Serial.print(alarmActive ? 1 : 0); Serial.print(' ');
  Serial.print(center);         Serial.print(' ');
  Serial.println(low);
}

void sendGpsTelemetry() {
  // G <valido> <sats> <lat> <lon> <km/h> <rumo> <bolinhas> <voltaAnt> <voltas> <distM>
  Serial.print(F("G "));
  Serial.print(gValid ? 1 : 0);   Serial.print(' ');
  Serial.print(gSats);            Serial.print(' ');
  Serial.print(gLat, 6);          Serial.print(' ');
  Serial.print(gLon, 6);          Serial.print(' ');
  Serial.print(gSpeedKmh, 1);     Serial.print(' ');
  Serial.print(gCourse, 0);       Serial.print(' ');
  Serial.print(bolinhas);         Serial.print(' ');
  Serial.print(lastLapBolinhas);  Serial.print(' ');
  Serial.print(lapCount);         Serial.print(' ');
  Serial.println(gDistM, 1);      // distancia ao ponto (m); <0 = sem ponto
}

// ---------------------------------------------------------------------
//  SETUP / LOOP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);                 // GPS NEO-6M (GPS6MV2/HW-248) na Serial1 (RX=D0)
  analogReference(DEFAULT);
  pinMode(ALERT_OUT_PIN, OUTPUT);
  digitalWrite(ALERT_OUT_PIN, HIGH);   // repouso (sem alerta)
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  loadEEPROM();
  applyConfig();
  loadGps();
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void loop() {
  handleSerial();
  readGps();                 // consome as sentencas NMEA da Serial1
  uint32_t now = millis();

  if (now - tSample >= sampleInterval) {
    tSample = now;
    lastVoltage = readVoltage();
    lastLambda  = toLambda(lastVoltage);
    pushSample(lastLambda);
    updateBolinha();         // conta bolinha por excursao do valor de sonda
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

  // IMPORTANTE (Arduino Micro / ATmega32U4): a "Serial" e USB (CDC). So enviamos
  // telemetria quando ha um PC com a porta ABERTA e com espaco no buffer de saida.
  // availableForWrite() retorna 0 quando nao ha host (app fechado / cabo so de
  // energia) OU quando o buffer encheu porque ninguem esta lendo. Sem esse guard,
  // o Serial.print BLOQUEIA o loop() esperando o buffer esvaziar (o que nunca
  // acontece sem o app) e o painel "acende e trava". Com o guard, o painel roda
  // 100% independente do PC.
  if (streaming && now - tTelem >= TELEM_MS && Serial.availableForWrite() > 0) {
    tTelem = now;
    sendTelemetry();
  }

  if (streaming && now - tGtelem >= 250 && Serial.availableForWrite() > 0) {
    tGtelem = now;
    sendGpsTelemetry();      // telemetria do GPS/bolinhas (~4 Hz) quando conectado
  }
}
