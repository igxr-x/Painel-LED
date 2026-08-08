# Painel LED Lambda — Arduino Micro + WS2812B 8×8 (CJMCU-64)

Sistema que lê a **sonda lambda**, calcula uma **média móvel**, e mostra o
resultado numa matriz de LED **8×8** como uma barra de colunas coloridas, onde
a **faixa de linhas** acesa indica a tendência (subindo / descendo / estável).
A configuração é feita por um **app Python** e fica gravada na **EEPROM** do
Arduino (funciona sozinho depois de configurado).

```
PAINEL LED/
├─ firmware/            → projeto PlatformIO (Arduino)
│  ├─ platformio.ini
│  └─ src/main.cpp
├─ app/                 → aplicativo configurador
│  ├─ app.py
│  └─ requirements.txt
├─ gravar.bat           → grava o firmware via PlatformIO
├─ abrir_app.bat        → abre o app (instala pyserial se faltar)
└─ README.md
```

---

## 1. Pinagem (Arduino Micro ↔ CJMCU-64 / WS2812B 8×8)

| Sinal              | Arduino Micro | Painel WS2812B (CJMCU-64) | Observação |
|--------------------|:-------------:|:-------------------------:|------------|
| Dados dos LEDs     | **D6**        | **DIN** (entrada de dados) | Use resistor de **330–470 Ω** em série no dado |
| GND (LEDs)         | **GND**       | **GND**                    | **Terra comum** obrigatório |
| +5V (LEDs)         | — (ver nota)  | **5V / VCC**               | Alimente por **fonte 5V externa**, não pelo USB |

| Sinal                      | Arduino Micro | Origem                    |
|----------------------------|:-------------:|---------------------------|
| Sinal da sonda lambda      | **A0**        | Saída 0–5 V do condicionador/wideband |
| GND da sonda               | **GND**       | Terra comum com o Arduino |

### Notas de energia (importante)
- 64 LEDs WS2812B podem puxar **até ~3,8 A** em branco pleno. Aqui só alguns
  acendem e o **brilho é limitado** (padrão 40/255), mas ainda assim **alimente
  o painel por uma fonte 5V externa** (2 A ou mais) — **não** pelo 5V do USB.
- Ligue **todos os GND juntos** (Arduino, fonte 5V e painel).
- Recomendado: **capacitor 1000 µF / 6,3 V+** entre 5V e GND do painel e o
  resistor de série no pino de dados (já citado).

### Sonda lambda
- O ADC do Micro aceita **0–5 V**. **Nunca** ligue a sonda direto se ela puder
  passar de 5 V — use o módulo condicionador/wideband com saída 0–5 V.
- A conversão tensão→lambda é uma **reta de 2 pontos** configurada no app
  (ex.: `1,0 V = 0,40 λ` e `4,0 V = 1,58 λ`).

---

## 2. Como gravar o firmware

1. Instale o **PlatformIO** (`pip install platformio`) ou a extensão
   *PlatformIO IDE* no VS Code.
2. Conecte o Arduino pelo USB.
3. Grave conforme a placa:
   - **Arduino Micro:** duplo clique em `gravar.bat` (ou `gravar.bat micro`).
   - **Arduino Nano (bootloader novo):** `gravar.bat nano`
   - **Arduino Nano (clone antigo, 57600):** `gravar.bat nanoold`

> **Nano funciona igual** — mesma fiação (D6 e A0). Só muda o alvo de gravação
> acima; a Nano usa chip CH340/FTDI, então no app pode ser preciso escolher a
> porta manualmente (o botão **Auto** já reconhece CH340/FTDI/CP210x).
> Para rodar via linha de comando: `pio run -e nano -t upload`.

---

## 3. Como usar o app

1. Duplo clique em **`abrir_app.bat`** (instala o `pyserial` na 1ª vez).
2. Clique em **Auto** para detectar a porta COM do Arduino e depois
   **Conectar** (a conexão é sempre manual).
3. Configure as abas e envie.

### Aba **Configurações** (ajustes que quase não mudam)
- **Cor de cada coluna** (8 colunas).
- **Alerta**: tempo/período (ms), tipo (**Estático** ou **Piscando**) e cor.
- **Calibração linear tensão↔lambda** (2 pontos).
- **Linhas por tendência**: quais linhas acendem quando a média móvel está
  **subindo**, **descendo** ou **estável** (1 = topo … 8 = base).
- **Variação mínima** (2 casas) para considerar **estável**.
- **Tempo da média móvel (ms)** e **taxa de amostragem (Hz)**.
- **Brilho global** (protege a fonte).
- Botão **“Enviar e Gravar”** → grava tudo na EEPROM.

### Aba **Rápido** (dia a dia)
- **Lambda por coluna** + botão **Interpolar** (preenche as colunas vazias
  linearmente entre as preenchidas — ex.: preencha col. 1, 4, 5, 6 e ele
  completa as do meio).
- **Leitura ao vivo**: tensão de entrada, lambda calculado, média móvel e
  tendência.
- **Lambda de alerta**: acima desse valor, os LEDs **apagados** acendem/piscam
  conforme o tipo/tempo/cor definidos na aba Configurações.
- **Prévia** da matriz 8×8 em tempo real.

---

## 4. Como a leitura vira imagem

- **Colunas**: cada coluna tem um valor de lambda. A coluna acende quando a
  média móvel atinge o valor dela → forma uma **barra** (col. 1 = menor λ).
- **Linhas**: a faixa de linhas acesa é escolhida pela **tendência** da média
  móvel (subindo/descendo/estável), conforme configurado.
- **Alerta**: se a média móvel ≥ *lambda de alerta*, os LEDs de fundo
  (apagados) acendem estáticos ou piscando.

---

## 5. Protocolo serial (115200 bps) — referência

App → Arduino (uma linha por comando):

```
PING                         -> PONG
GET                          -> devolve "CFG ..." com toda a config
COLOR i r g b                cor da coluna i (0..7)
COLLAMBDA i valor            lambda da coluna i
ALERT tempo tipo             tipo: 0=estatico 1=piscando
ALERTCOLOR r g b             cor do alerta
CALIB v1 l1 v2 l2            reta de calibracao
TRENDROWS rise fall stab     bitmask 8 bits de linhas por tendencia
STABLE thresh                variacao minima p/ estavel
MAVG ms                      tempo da media movel
SRATE hz                     taxa de amostragem
ALARM lambda                 lambda de disparo do alerta
BRIGHT 0-255                 brilho global
SAVE                         grava tudo na EEPROM
STREAM 0|1                   liga/desliga telemetria
```

Arduino → App (telemetria contínua, ~10 Hz):

```
D <tensao> <lambda> <mediaMovel> <trend> <colunas> <alarme>
     trend: 1=subindo  0=estavel  -1=descendo
```

> **Ajuste do mapeamento:** o CJMCU-64 costuma ser ligado em *serpentina*
> (zig-zag). Se a imagem sair espelhada/torta, mude `#define SERPENTINE` em
> `firmware/src/main.cpp` (true/false) e/ou a função `XY()`.
