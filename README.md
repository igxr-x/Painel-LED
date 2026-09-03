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

| Sinal                      | Arduino Micro | Destino                   |
|----------------------------|:-------------:|---------------------------|
| Saída de alerta (negativo/GND) | **D10**   | Relé / buzina via transistor |

| Sinal (GPS GPS6MV2 / HW-248 / NEO-6M) | Arduino Micro | Observação |
|---------------------------------------|:-------------:|------------|
| **TX** do GPS                         | **D0 (RX1)**  | Serial1 de hardware — só o **Micro** tem, não conflita com o USB |
| **VCC** do GPS                        | **5V**        | O módulo tem regulador próprio p/ 3,3 V |
| **GND** do GPS                        | **GND**       | Terra comum |

> O RX do GPS **não precisa** ser ligado (leitura só). O **Nano não serve** para
> o GPS: só tem uma serial de hardware (ocupada pelo USB) e o SoftwareSerial
> quebra com o FastLED. O modo prova é **exclusivo do Micro**.

### Saída de alerta (D10)
- O **negativo (GND) do alerta sai no pino D10**.
- **Repouso:** D10 fica em **HIGH**. **Em alerta:** D10 vai para **LOW**
  (nível baixo = negativo/GND) — lógica **invertida**.
- Não acione relé/buzina direto pelo pino: use um **transistor** (o pino do
  Arduino não deve chavear cargas indutivas diretamente).

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
- **Motor Diesel (sonda invertida)** — acende quando o valor fica abaixo do
  configurado (ligado por padrão).
- **LED verde central** em repouso (com cor configurável).
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

> **Motor Diesel (padrão):** a sonda trabalha de valores **maiores para
> menores**, então a lógica é **invertida** — colunas e alerta acendem quando o
> valor fica **ABAIXO** do configurado. Desmarque *Motor Diesel* na aba
> Configurações para voltar à lógica normal (acende quando fica acima).

- **Colunas**: cada coluna tem um valor de lambda. Em modo diesel a coluna
  acende quando a média móvel fica **≤** o valor dela.
- **LED verde central (2×2)**: fica aceso enquanto o painel está **em repouso**
  (valor acima do primeiro LED a acender / nenhuma coluna ligada).
- **Linhas**: a faixa de linhas acesa é escolhida pela **tendência** da média
  móvel (subindo/descendo/estável), conforme configurado.
- **Alerta**: em modo diesel, se a média móvel **≤** *lambda de alerta*
  (ex.: 1,33 ou menos), os LEDs de fundo (apagados) acendem estáticos ou
  piscando — sem apagar o LED verde central.

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
DIESEL 0|1                   0=normal (>=)  1=diesel/invertido (<=)
CENTER enable r g b          LED verde central (repouso)
MAP serp flipx flipy transp  mapeamento fisico da matriz
TEST 0|1                     padrao de teste p/ ajustar o mapeamento
SAVE                         grava tudo na EEPROM (config + GPS)
STREAM 0|1                   liga/desliga telemetria

--- Modo prova / GPS ---
GGET                         devolve "GCFG ..." (config do GPS)
GPSEN 0|1                    liga/desliga o contador de bolinhas
GPSLINE latA lonA latB lonB  linha de chegada (segmento)
BOLIM lambda limite          lambda limite da bolinha + nro p/ penalizar
BHYST valor                  histerese p/ rearmar a contagem
BROW 0..7                    linha da borda reservada ao contador
BCOL idx r g b               cor: idx 0=normal 1=atencao 2=estourado
BLAP minLapMs minSpeedKmh    tempo min. entre voltas + velocidade minima
BRESET                       zera as bolinhas da volta atual
LAPRESET                     zera bolinhas + contador de voltas
```

Arduino → App (telemetria contínua, ~10 Hz):

```
D <tensao> <lambda> <mediaMovel> <trend> <colMask> <alarme> <central>
     trend:   1=subindo  0=estavel  -1=descendo
     colMask: bitmask das 8 colunas acesas (bit0=col1)
     central: 1 = LED verde central aceso (em repouso)

G <valido> <sats> <lat> <lon> <km/h> <rumo> <bolinhas> <voltaAnt> <voltas>   (~4 Hz, só com GPS ligado)
     valido:  1 = fix de GPS válido
```

## 6. Corrigir LEDs invertidos / zig-zag (mapeamento)

O CJMCU-64 pode ter a fiação em *serpentina* (zig-zag) e a entrada de dados em
qualquer canto — por isso a imagem pode sair “com uma linha vindo e outra
indo”. Ajuste **sem recompilar**, pelo app (aba **Configurações → Mapeamento**):

1. Conecte e marque **MODO TESTE**. O painel mostra:
   - **linha de cima** = vermelha, **coluna da esquerda** = azul,
   - **canto superior esquerdo** = branco, **canto superior direito** = verde.
2. Marque/desmarque **Serpentina, Espelhar X, Espelhar Y, Transpor** até:
   - a linha **vermelha** ficar **reta no topo** e
   - a coluna **azul** ficar **reta na esquerda** (branco no canto sup. esq.).
   As mudanças aplicam **ao vivo**.
3. Desligue o **MODO TESTE** e clique **“Enviar e Gravar”** para salvar.

> Combinações típicas do CJMCU-64: só **Serpentina**; ou **Serpentina + Espelhar
> Y**; ou **Transpor + Serpentina**. Vá testando — são poucos cliques.

## 7. Modo prova — GPS + contador de "bolinhas"

Conta as **bolinhas** (cada vez que a sonda passa do valor permitido pela prova) e
mostra numa **linha da borda** do painel reservada só pra isso — o resto do painel
continua funcionando igual. A cada **linha de chegada** cruzada (via GPS), o
contador **zera** (nova volta). Tudo roda **no próprio Arduino** (funciona sem
notebook no caminhão); o app só configura e monitora.

- **Config em EEPROM separada:** ativar o GPS **não apaga** sua configuração atual
  (cores, calibração, lambdas). Se algo der errado, `LOAD` recarrega tudo.
- **Como conta 1 bolinha:** cada **excursão** do valor de sonda além do *lambda
  limite* (independente do alerta visual). Uma histerese evita contar a mesma
  excursão várias vezes quando o valor treme na borda.
- **No painel:** 1 LED por bolinha na linha reservada. **Verde** normal, **âmbar**
  quando falta 1 pro limite, **vermelho piscando** ao estourar (≥ limite, padrão 6).

### Aba **Prova / GPS** (no app)
1. Marque **Ligar contador de bolinhas**.
2. Ajuste **lambda limite**, **limite de bolinhas** (padrão 6), a **linha
   reservada** (base/topo) e as cores.
3. Defina a **linha de chegada**:
   - **Auto-detectar pela posição** escolhe a pista mais próxima do banco
     (`app/tracks.json`); **ou** selecione a pista na lista; **ou**
   - **Capturar linha aqui** (o mais preciso): pare/passe **em cima da linha** e
     clique — ele gera o segmento perpendicular ao seu rumo (largura configurável).
     Depois **Salvar linha nesta pista** grava no banco pra próxima vez.
4. **Enviar e gravar configurações do GPS**.
5. Acompanhe **GPS ao vivo** (sinal/sats, posição, velocidade, bolinhas da volta,
   volta anterior, nº de voltas). Botões **Zerar bolinhas / Zerar voltas**.

> **Precisão:** o NEO-6M tem ~2,5 m de erro e sai de fábrica a 1 Hz. A detecção
> usa **cruzamento de segmento** (trecho percorrido × linha de chegada), então
> funciona mesmo a 1 Hz, mas a **captura no local** é sempre mais confiável que as
> coordenadas aproximadas do banco. Há filtros de **velocidade mínima** e **tempo
> mínimo entre voltas** pra não contar volta parado no box.

> **Não existe** um banco oficial/global com a linha de chegada exata de todas as
> pistas — por isso o banco embutido traz só pontos **aproximados** (pra
> auto-detectar o nome) e o fluxo recomendado é **capturar a sua linha** na pista.
