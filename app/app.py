#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Painel LED Lambda - Configurador
=================================
App para configurar o Arduino Micro que controla a matriz WS2812B 8x8
(CJMCU-64) a partir da sonda lambda.

- Deteccao automatica da porta COM do Arduino (conexao manual)
- Aba "Configuracoes" (estaticas, mudam pouco)
- Aba "Rapido" (ajustes do dia-a-dia + telemetria ao vivo)

Requisitos:  pip install pyserial      (Tkinter ja vem com o Python)
"""

import queue
import threading
import time
from collections import deque
import tkinter as tk
from tkinter import ttk, colorchooser, messagebox

import serial
import serial.tools.list_ports

BAUD = 115200
NUM_COLS = 8
NUM_ROWS = 8
ADC_VREF = 5.0   # tensao de referencia do ADC (5V no Arduino Micro) - usado no simulador

# VID/PID que indicam um Arduino (prioridade na deteccao automatica)
# Micro/genuinos (USB nativo) + chips seriais comuns das Nano (CH340/FTDI/CP210x)
ARDUINO_VIDPID = [
    (0x2341, 0x0037), (0x2341, 0x8037), (0x2341, 0x0237),  # Arduino Micro
    (0x2A03, 0x0037), (0x2A03, 0x8037),                    # Arduino Micro (arduino.org)
    (0x2341, 0x0043), (0x2341, 0x0001),                    # Uno/Nano genuinos
    (0x1A86, 0x7523), (0x1A86, 0x5523),                    # CH340/CH341 (clones Nano)
    (0x0403, 0x6001),                                      # FTDI FT232 (Nano antiga)
    (0x10C4, 0xEA60),                                      # CP2102
]
# Palavras que aparecem na descricao de placas Arduino / conversores seriais
ARDUINO_DESC = ("arduino", "micro", "ch340", "ch341", "usb-serial",
                "usb serial", "ftdi", "cp210", "wch")


# =====================================================================
#  CAMADA SERIAL
# =====================================================================
class SerialLink:
    def __init__(self, on_line):
        self.ser = None
        self.on_line = on_line
        self._reader = None
        self._stop = threading.Event()

    @staticmethod
    def list_ports():
        """Lista portas, Arduinos conhecidos primeiro."""
        ports = list(serial.tools.list_ports.comports())
        def score(p):
            if (p.vid, p.pid) in ARDUINO_VIDPID:
                return 0
            desc = (p.description or "").lower()
            if any(w in desc for w in ARDUINO_DESC):
                return 1
            return 2
        ports.sort(key=score)
        return ports

    @staticmethod
    def auto_detect():
        for p in SerialLink.list_ports():
            if (p.vid, p.pid) in ARDUINO_VIDPID:
                return p.device
            desc = (p.description or "").lower()
            if any(w in desc for w in ARDUINO_DESC):
                return p.device
        return None

    def open(self, port):
        self.close()
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        time.sleep(1.8)  # o Micro reinicia ao abrir a porta
        self._stop.clear()
        self._reader = threading.Thread(target=self._loop, daemon=True)
        self._reader.start()

    def close(self):
        self._stop.set()
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def is_open(self):
        return self.ser is not None and self.ser.is_open

    def send(self, line):
        if self.is_open():
            self.ser.write((line + "\n").encode("ascii", "ignore"))
            self.ser.flush()

    def _loop(self):
        buf = b""
        while not self._stop.is_set() and self.ser:
            try:
                data = self.ser.read(128)
            except Exception:
                break
            if data:
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    txt = raw.decode("ascii", "ignore").strip()
                    if txt:
                        self.on_line(txt)
            else:
                time.sleep(0.01)


# =====================================================================
#  APP / GUI
# =====================================================================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Painel LED Lambda - Configurador")
        self.geometry("880x760")

        self.rx = queue.Queue()
        self.link = SerialLink(lambda l: self.rx.put(l))

        # ----- estado dos widgets -----
        self.col_colors = ["#00ff00"] * NUM_COLS   # cor por coluna
        self.col_lambda_vars = []                   # lambda por coluna
        self.rows_vars = {"rise": [], "fall": [], "stab": []}

        self._build_toolbar()
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=4)
        self.tab_fast = ttk.Frame(nb)
        self.tab_cfg = ttk.Frame(nb)
        self.tab_sim = ttk.Frame(nb)
        nb.add(self.tab_fast, text="  Rapido  ")
        nb.add(self.tab_cfg, text="  Configuracoes  ")
        nb.add(self.tab_sim, text="  Simulador  ")
        # cfg/fast criam os widgets/variaveis usados pelo simulador -> construir antes
        self._build_cfg_tab()
        self._build_fast_tab()
        self._build_sim_tab()

        self.after(60, self._poll_rx)
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.refresh_ports()

    # ---------------- Toolbar / conexao ----------------
    def _build_toolbar(self):
        bar = ttk.Frame(self)
        bar.pack(fill="x", padx=8, pady=6)
        ttk.Label(bar, text="Porta COM:").pack(side="left")
        self.port_cb = ttk.Combobox(bar, width=34, state="readonly")
        self.port_cb.pack(side="left", padx=4)
        ttk.Button(bar, text="Atualizar", command=self.refresh_ports).pack(side="left", padx=2)
        ttk.Button(bar, text="Auto", command=self.auto_port).pack(side="left", padx=2)
        self.btn_conn = ttk.Button(bar, text="Conectar", command=self.toggle_conn)
        self.btn_conn.pack(side="left", padx=6)
        self.status = ttk.Label(bar, text="Desconectado", foreground="#b00")
        self.status.pack(side="left", padx=6)
        # Botao de salvar as configuracoes junto da conexao (sempre visivel)
        ttk.Button(bar, text="Enviar e Gravar configuracoes (SAVE)",
                   command=self.send_all_static).pack(side="right", padx=6)

    def refresh_ports(self):
        self.ports = SerialLink.list_ports()
        vals = ["{}  -  {}".format(p.device, p.description) for p in self.ports]
        self.port_cb["values"] = vals
        if vals and not self.port_cb.get():
            self.port_cb.current(0)

    def auto_port(self):
        dev = SerialLink.auto_detect()
        if dev:
            for i, p in enumerate(self.ports):
                if p.device == dev:
                    self.port_cb.current(i)
                    break
            messagebox.showinfo("Auto", "Arduino detectado em {}".format(dev))
        else:
            messagebox.showwarning("Auto", "Arduino nao detectado automaticamente.")

    def toggle_conn(self):
        if self.link.is_open():
            self.link.close()
            self.btn_conn.config(text="Conectar")
            self.status.config(text="Desconectado", foreground="#b00")
            return
        idx = self.port_cb.current()
        if idx < 0 or idx >= len(self.ports):
            messagebox.showwarning("Conexao", "Selecione uma porta COM.")
            return
        try:
            self.link.open(self.ports[idx].device)
        except Exception as e:
            messagebox.showerror("Conexao", "Falha ao abrir a porta:\n{}".format(e))
            return
        self.btn_conn.config(text="Desconectar")
        self.status.config(text="Conectado " + self.ports[idx].device, foreground="#080")
        self.link.send("PING")
        self.after(400, lambda: self.link.send("GET"))

    # ---------------- ABA CONFIGURACOES ----------------
    def _build_cfg_tab(self):
        # Area rolavel: canvas + scrollbar para nao cortar o botao SALVAR
        outer = self.tab_cfg
        canvas = tk.Canvas(outer, highlightthickness=0)
        vbar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vbar.set)
        vbar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        f = ttk.Frame(canvas)
        win = canvas.create_window((0, 0), window=f, anchor="nw")
        f.bind("<Configure>",
               lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
                    lambda e: canvas.itemconfigure(win, width=e.width))

        def _on_wheel(e):
            canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")
        # so ativa a roda do mouse quando o cursor esta sobre esta aba
        f.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>", _on_wheel))
        f.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))

        # Cores das colunas
        g1 = ttk.LabelFrame(f, text="Cor de cada coluna")
        g1.pack(fill="x", padx=8, pady=6)
        self.color_btns = []
        for i in range(NUM_COLS):
            b = tk.Button(g1, text=str(i + 1), width=4, bg=self.col_colors[i],
                          command=lambda i=i: self.pick_color(i))
            b.grid(row=0, column=i, padx=3, pady=6)
            self.color_btns.append(b)

        # Alerta
        g2 = ttk.LabelFrame(f, text="Alerta")
        g2.pack(fill="x", padx=8, pady=6)
        ttk.Label(g2, text="Periodo do pisca (ms):").grid(row=0, column=0, sticky="e", padx=4, pady=4)
        self.alert_time = tk.StringVar(value="300")
        ttk.Entry(g2, textvariable=self.alert_time, width=8).grid(row=0, column=1, padx=4)
        ttk.Label(g2, text="Tipo:").grid(row=0, column=2, sticky="e", padx=4)
        self.alert_type = tk.StringVar(value="Piscando")
        ttk.Combobox(g2, textvariable=self.alert_type, width=10, state="readonly",
                     values=["Estatico", "Piscando"]).grid(row=0, column=3, padx=4)
        ttk.Label(g2, text="Cor do alerta:").grid(row=0, column=4, sticky="e", padx=4)
        self.alert_color = "#ff0000"
        self.alert_btn = tk.Button(g2, text="   ", width=4, bg=self.alert_color,
                                   command=self.pick_alert_color)
        self.alert_btn.grid(row=0, column=5, padx=4)
        ttk.Label(g2, text="Tempo minimo ligado (ms):").grid(row=1, column=0, sticky="e", padx=4, pady=4)
        self.alert_hold = tk.StringVar(value="3000")
        ttk.Entry(g2, textvariable=self.alert_hold, width=8).grid(row=1, column=1, padx=4)
        ttk.Label(g2, text="(uma vez disparado, fica alertando por no minimo esse tempo)",
                  foreground="#666").grid(row=1, column=2, columnspan=4, sticky="w", padx=4)

        # Calibracao tensao/lambda
        g3 = ttk.LabelFrame(f, text="Calibracao linear  Tensao <-> Lambda (2 pontos)")
        g3.pack(fill="x", padx=8, pady=6)
        self.v1 = tk.StringVar(value="1.0"); self.l1 = tk.StringVar(value="0.40")
        self.v2 = tk.StringVar(value="4.0"); self.l2 = tk.StringVar(value="1.58")
        ttk.Label(g3, text="Ponto 1  Tensao (V):").grid(row=0, column=0, sticky="e", padx=4, pady=4)
        ttk.Entry(g3, textvariable=self.v1, width=8).grid(row=0, column=1, padx=4)
        ttk.Label(g3, text="= Lambda:").grid(row=0, column=2, sticky="e", padx=4)
        ttk.Entry(g3, textvariable=self.l1, width=8).grid(row=0, column=3, padx=4)
        ttk.Label(g3, text="Ponto 2  Tensao (V):").grid(row=1, column=0, sticky="e", padx=4, pady=4)
        ttk.Entry(g3, textvariable=self.v2, width=8).grid(row=1, column=1, padx=4)
        ttk.Label(g3, text="= Lambda:").grid(row=1, column=2, sticky="e", padx=4)
        ttk.Entry(g3, textvariable=self.l2, width=8).grid(row=1, column=3, padx=4)

        # Linhas por tendencia  (visualizacao em coluna: topo -> base)
        g4 = ttk.LabelFrame(f, text="Linhas acesas por tendencia da media movel  (topo -> base)")
        g4.pack(fill="x", padx=8, pady=6)
        trends = [("Subindo", "rise"), ("Estavel", "stab"), ("Descendo", "fall")]
        # cabecalho: uma coluna por tendencia
        ttk.Label(g4, text="Linha").grid(row=0, column=0, padx=6, pady=(4, 2))
        for c, (txt, _key) in enumerate(trends):
            ttk.Label(g4, text=txt, font=("Segoe UI", 9, "bold")).grid(
                row=0, column=1 + c, padx=12, pady=(4, 2))
        # 8 linhas empilhadas verticalmente (1 = topo ... 8 = base)
        for y in range(NUM_ROWS):
            tag = "  (topo)" if y == 0 else ("  (base)" if y == NUM_ROWS - 1 else "")
            ttk.Label(g4, text="{}{}".format(y + 1, tag)).grid(
                row=1 + y, column=0, sticky="e", padx=6, pady=1)
            for c, (_txt, key) in enumerate(trends):
                v = tk.IntVar()
                ttk.Checkbutton(g4, variable=v).grid(row=1 + y, column=1 + c, padx=12, pady=1)
                self.rows_vars[key].append(v)
        # defaults
        for y in (0, 1, 2): self.rows_vars["rise"][y].set(1)
        for y in (5, 6, 7): self.rows_vars["fall"][y].set(1)
        for y in (3, 4, 5): self.rows_vars["stab"][y].set(1)

        # Media movel / amostragem / estabilidade
        g5 = ttk.LabelFrame(f, text="Media movel e amostragem")
        g5.pack(fill="x", padx=8, pady=6)
        self.stable_thresh = tk.StringVar(value="0.02")
        self.mavg_time = tk.StringVar(value="500")
        self.sample_rate = tk.StringVar(value="50")
        ttk.Label(g5, text="Variacao min. estavel (lambda):").grid(row=0, column=0, sticky="e", padx=4, pady=4)
        ttk.Entry(g5, textvariable=self.stable_thresh, width=8).grid(row=0, column=1, padx=4)
        ttk.Label(g5, text="Tempo media movel (ms):").grid(row=0, column=2, sticky="e", padx=4)
        ttk.Entry(g5, textvariable=self.mavg_time, width=8).grid(row=0, column=3, padx=4)
        ttk.Label(g5, text="Taxa de amostragem (Hz):").grid(row=0, column=4, sticky="e", padx=4)
        ttk.Entry(g5, textvariable=self.sample_rate, width=8).grid(row=0, column=5, padx=4)

        # Brilho
        g6 = ttk.LabelFrame(f, text="Brilho global do painel (protege a fonte)")
        g6.pack(fill="x", padx=8, pady=6)
        self.brightness = tk.IntVar(value=40)
        ttk.Scale(g6, from_=1, to=255, variable=self.brightness,
                  orient="horizontal", length=300).pack(side="left", padx=8, pady=6)
        ttk.Label(g6, textvariable=self.brightness).pack(side="left")

        # Modo diesel (sonda invertida) + LED verde central
        g7 = ttk.LabelFrame(f, text="Tipo de motor e LED central")
        g7.pack(fill="x", padx=8, pady=6)
        self.diesel_mode = tk.IntVar(value=1)
        ttk.Checkbutton(g7, variable=self.diesel_mode,
                        text="Motor Diesel (sonda invertida: acende quando o valor "
                             "fica ABAIXO do configurado)").grid(
            row=0, column=0, columnspan=4, sticky="w", padx=6, pady=4)
        self.center_enable = tk.IntVar(value=1)
        ttk.Checkbutton(g7, variable=self.center_enable,
                        text="LED verde central quando em repouso (acima do 1o LED)").grid(
            row=1, column=0, columnspan=3, sticky="w", padx=6, pady=4)
        ttk.Label(g7, text="Cor central:").grid(row=1, column=3, sticky="e", padx=4)
        self.center_color = "#00ff00"
        self.center_btn = tk.Button(g7, text="   ", width=4, bg=self.center_color,
                                    command=self.pick_center_color)
        self.center_btn.grid(row=1, column=4, padx=4)

        # Mapeamento fisico da matriz (corrige LEDs "invertidos"/zig-zag)
        g8 = ttk.LabelFrame(f, text="Mapeamento do painel  (corrige LEDs invertidos / zig-zag)")
        g8.pack(fill="x", padx=8, pady=6)
        self.map_serp = tk.IntVar(value=1)
        self.map_flipx = tk.IntVar(value=0)
        self.map_flipy = tk.IntVar(value=0)
        self.map_transp = tk.IntVar(value=0)
        opts = [("Serpentina (zig-zag)", self.map_serp),
                ("Espelhar X", self.map_flipx),
                ("Espelhar Y", self.map_flipy),
                ("Transpor (linha/coluna)", self.map_transp)]
        for c, (txt, var) in enumerate(opts):
            ttk.Checkbutton(g8, text=txt, variable=var,
                            command=self.send_map).grid(row=0, column=c, sticky="w", padx=6, pady=4)
        self.test_on = tk.IntVar(value=0)
        ttk.Checkbutton(g8, text="MODO TESTE (topo=vermelho, esquerda=azul, canto=branco)",
                        variable=self.test_on, command=self.toggle_test).grid(
            row=1, column=0, columnspan=4, sticky="w", padx=6, pady=4)
        ttk.Label(g8, text="Ligue o teste e marque as opcoes acima ate a linha "
                           "vermelha ficar reta no TOPO e a azul reta na ESQUERDA.",
                  foreground="#666").grid(row=2, column=0, columnspan=4, sticky="w", padx=6)

    # ---------------- ABA RAPIDO ----------------
    def _build_fast_tab(self):
        f = self.tab_fast

        g1 = ttk.LabelFrame(f, text="Valor de lambda (sonda) por coluna")
        g1.pack(fill="x", padx=8, pady=6)
        for i in range(NUM_COLS):
            ttk.Label(g1, text="Col {}".format(i + 1)).grid(row=0, column=i, padx=6)
            v = tk.StringVar(value="{:.2f}".format(0.70 + i * 0.10))
            ttk.Entry(g1, textvariable=v, width=7).grid(row=1, column=i, padx=6, pady=4)
            self.col_lambda_vars.append(v)
        ttk.Button(g1, text="Interpolar colunas vazias",
                   command=self.interpolate_columns).grid(row=2, column=0, columnspan=4,
                                                          sticky="w", padx=4, pady=6)
        ttk.Label(g1, text="(preencha algumas e deixe vazias no meio para interpolar)",
                  foreground="#666").grid(row=2, column=4, columnspan=4, sticky="w")

        g2 = ttk.LabelFrame(f, text="Alerta rapido")
        g2.pack(fill="x", padx=8, pady=6)
        ttk.Label(g2, text="Lambda de alerta (diesel: dispara com valor ABAIXO):").grid(
            row=0, column=0, sticky="e", padx=4, pady=6)
        self.alarm_lambda = tk.StringVar(value="1.33")
        ttk.Entry(g2, textvariable=self.alarm_lambda, width=8).grid(row=0, column=1, padx=4)
        ttk.Button(g2, text="Enviar ajustes rapidos",
                   command=self.send_fast).grid(row=0, column=2, padx=16)

        # Telemetria ao vivo
        g3 = ttk.LabelFrame(f, text="Leitura ao vivo")
        g3.pack(fill="x", padx=8, pady=6)
        self.lbl_volt = tk.StringVar(value="--")
        self.lbl_lam = tk.StringVar(value="--")
        self.lbl_mavg = tk.StringVar(value="--")
        self.lbl_trend = tk.StringVar(value="--")
        big = ("Segoe UI", 16, "bold")
        ttk.Label(g3, text="Tensao entrada:").grid(row=0, column=0, sticky="e", padx=6, pady=6)
        ttk.Label(g3, textvariable=self.lbl_volt, font=big, foreground="#06c").grid(row=0, column=1, sticky="w")
        ttk.Label(g3, text="V").grid(row=0, column=2, sticky="w")
        ttk.Label(g3, text="Lambda calculado:").grid(row=0, column=3, sticky="e", padx=6)
        ttk.Label(g3, textvariable=self.lbl_lam, font=big, foreground="#080").grid(row=0, column=4, sticky="w")
        ttk.Label(g3, text="Media movel:").grid(row=1, column=0, sticky="e", padx=6, pady=6)
        ttk.Label(g3, textvariable=self.lbl_mavg, font=big).grid(row=1, column=1, sticky="w")
        ttk.Label(g3, text="Tendencia:").grid(row=1, column=3, sticky="e", padx=6)
        ttk.Label(g3, textvariable=self.lbl_trend, font=big).grid(row=1, column=4, sticky="w")

        # Preview simples da matriz 8x8
        g4 = ttk.LabelFrame(f, text="Previa do painel (colunas x tendencia)")
        g4.pack(fill="both", expand=True, padx=8, pady=6)
        self.canvas = tk.Canvas(g4, width=8 * 34, height=8 * 34, bg="#111", highlightthickness=0)
        self.canvas.pack(padx=8, pady=8)
        self.cells = [[None] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(8):
                self.cells[y][x] = self.canvas.create_oval(
                    x * 34 + 4, y * 34 + 4, x * 34 + 30, y * 34 + 30,
                    fill="#222", outline="#333")

        # Log
        g5 = ttk.LabelFrame(f, text="Log serial")
        g5.pack(fill="x", padx=8, pady=6)
        self.logbox = tk.Text(g5, height=5, bg="#0b0b0b", fg="#0f0")
        self.logbox.pack(fill="x", padx=4, pady=4)

    # ---------------- ABA SIMULADOR ----------------
    def _build_sim_tab(self):
        f = self.tab_sim

        top = ttk.LabelFrame(f, text="Simulador da sonda  (usa as configuracoes atuais + media movel)")
        top.pack(fill="x", padx=8, pady=6)

        ttk.Label(top, text="Tensao da sonda (V):").grid(row=0, column=0, sticky="e", padx=6, pady=8)
        self.sim_volt = tk.DoubleVar(value=1.0)
        ttk.Scale(top, from_=0.0, to=ADC_VREF, variable=self.sim_volt, orient="horizontal",
                  length=380).grid(row=0, column=1, columnspan=3, padx=6, sticky="w")
        ttk.Button(top, text="Zerar media movel", command=self._sim_reset).grid(row=0, column=4, padx=8)

        big = ("Segoe UI", 15, "bold")
        self.sim_lbl_volt = tk.StringVar(value="--")
        self.sim_lbl_lam = tk.StringVar(value="--")
        self.sim_lbl_mavg = tk.StringVar(value="--")
        self.sim_lbl_trend = tk.StringVar(value="--")
        self.sim_lbl_alarm = tk.StringVar(value="--")
        ttk.Label(top, text="Tensao:").grid(row=1, column=0, sticky="e", padx=6, pady=4)
        ttk.Label(top, textvariable=self.sim_lbl_volt, font=big, foreground="#06c").grid(row=1, column=1, sticky="w")
        ttk.Label(top, text="V").grid(row=1, column=2, sticky="w")
        ttk.Label(top, text="Lambda (instantaneo):").grid(row=1, column=3, sticky="e", padx=6)
        ttk.Label(top, textvariable=self.sim_lbl_lam, font=big, foreground="#080").grid(row=1, column=4, sticky="w")
        ttk.Label(top, text="Media movel:").grid(row=2, column=0, sticky="e", padx=6, pady=4)
        ttk.Label(top, textvariable=self.sim_lbl_mavg, font=big, foreground="#c60").grid(row=2, column=1, sticky="w")
        ttk.Label(top, text="Tendencia:").grid(row=2, column=3, sticky="e", padx=6)
        ttk.Label(top, textvariable=self.sim_lbl_trend, font=big).grid(row=2, column=4, sticky="w")
        ttk.Label(top, text="Alerta:").grid(row=3, column=0, sticky="e", padx=6, pady=4)
        ttk.Label(top, textvariable=self.sim_lbl_alarm, font=big, foreground="#b00").grid(row=3, column=1, sticky="w")
        ttk.Label(top, text="(a barra/alerta/tendencia usam a MEDIA MOVEL, igual ao painel real)",
                  foreground="#666").grid(row=4, column=0, columnspan=5, sticky="w", padx=6, pady=(2, 4))

        pf = ttk.LabelFrame(f, text="Previa do painel simulado")
        pf.pack(fill="both", expand=True, padx=8, pady=6)
        self.sim_canvas = tk.Canvas(pf, width=8 * 34, height=8 * 34, bg="#111", highlightthickness=0)
        self.sim_canvas.pack(padx=8, pady=8)
        self.sim_cells = [[None] * 8 for _ in range(8)]
        for y in range(8):
            for x in range(8):
                self.sim_cells[y][x] = self.sim_canvas.create_oval(
                    x * 34 + 4, y * 34 + 4, x * 34 + 30, y * 34 + 30,
                    fill="#222", outline="#333")

        # estado da simulacao da media movel (buffer circular, como o firmware)
        self._sim_ring = deque()
        self._sim_windowN = 0
        self._sim_mavg = None
        self._sim_trend = 0
        self._sim_trend_ref = None
        self._sim_last = None       # instante do ultimo tick (ms)
        self._sim_last_trend = None # instante da ultima avaliacao de tendencia (ms)
        self._sim_hold_until = 0.0  # ms ate quando o alerta fica ligado (tempo minimo)
        self.after(40, self._sim_tick)

    def _sim_reset(self):
        """Zera o buffer da media movel (a media 'salta' para o valor atual)."""
        self._sim_ring.clear()
        self._sim_mavg = None
        self._sim_trend = 0
        self._sim_trend_ref = None
        self._sim_hold_until = 0.0

    def _sim_float(self, var, default=0.0):
        """Le uma StringVar/DoubleVar como float, tolerando virgula e campo vazio."""
        try:
            return float(str(var.get()).strip().replace(",", "."))
        except (ValueError, AttributeError):
            return default

    def _sim_lambda(self, v):
        """Converte tensao -> lambda pela reta de calibracao (igual ao firmware)."""
        v1, l1 = self._sim_float(self.v1, 1.0), self._sim_float(self.l1, 0.40)
        v2, l2 = self._sim_float(self.v2, 4.0), self._sim_float(self.l2, 1.58)
        dv = v2 - v1
        if abs(dv) < 1e-6:
            return l1
        return l1 + (v - v1) * (l2 - l1) / dv

    def _sim_tick(self):
        """Avanca a simulacao: alimenta a media movel com o valor atual do slider
        conforme a taxa de amostragem, recalcula media/tendencia e redesenha."""
        now = time.perf_counter() * 1000.0
        if self._sim_last is None:
            self._sim_last = now
            self._sim_last_trend = now

        # parametros de amostragem/media (mesma matematica do applyConfig do firmware)
        srate = min(max(self._sim_float(self.sample_rate, 50), 1.0), 500.0)
        sample_interval = max(1000.0 / srate, 2.0)
        windowN = int(self._sim_float(self.mavg_time, 500) / sample_interval)
        windowN = min(max(windowN, 1), 200)   # MAX_WINDOW = 200 no firmware
        if windowN != self._sim_windowN:
            self._sim_windowN = windowN
            self._sim_ring = deque(self._sim_ring, maxlen=windowN)

        lam = self._sim_lambda(self._sim_float(self.sim_volt, 0.0))

        # empurra as amostras que "caberiam" no tempo decorrido desde o ultimo tick
        n = int((now - self._sim_last) / sample_interval)
        if n > windowN:
            n = windowN            # nao adianta empurrar mais que a janela inteira
        for _ in range(n):
            self._sim_ring.append(lam)
        if n:
            self._sim_last += n * sample_interval

        self._sim_mavg = (sum(self._sim_ring) / len(self._sim_ring)) if self._sim_ring else lam

        # tendencia a cada 250 ms (TREND_MS), comparando com o limiar de estabilidade
        if now - self._sim_last_trend >= 250:
            self._sim_last_trend = now
            thr = self._sim_float(self.stable_thresh, 0.02)
            ref = self._sim_trend_ref if self._sim_trend_ref is not None else self._sim_mavg
            d = self._sim_mavg - ref
            self._sim_trend = 1 if d > thr else (-1 if d < -thr else 0)
            self._sim_trend_ref = self._sim_mavg

        self._sim_render(lam, self._sim_mavg, self._sim_trend)
        self.after(40, self._sim_tick)

    def _sim_render(self, lam, mavg, trend):
        """Desenha o painel simulado a partir da media movel (igual ao firmware)."""
        diesel = self.diesel_mode.get()
        colmask = 0
        for x in range(NUM_COLS):
            cl = self._sim_float(self.col_lambda_vars[x], None)
            if cl is None:
                continue
            lit = (mavg <= cl) if diesel else (mavg >= cl)
            if lit:
                colmask |= (1 << x)

        al = self._sim_float(self.alarm_lambda, None)
        raw_alarm = False if al is None else ((mavg <= al) if diesel else (mavg >= al))
        # tempo minimo ligado (hold): mantem o alerta ativo apos disparar
        now = time.perf_counter() * 1000.0
        if raw_alarm:
            self._sim_hold_until = now + self._sim_float(self.alert_hold, 0.0)
        alarm = raw_alarm or (now < self._sim_hold_until)
        center = 1 if (self.center_enable.get() and colmask == 0) else 0

        self.sim_lbl_volt.set("{:.3f}".format(self._sim_float(self.sim_volt, 0.0)))
        self.sim_lbl_lam.set("{:.3f}".format(lam))
        self.sim_lbl_mavg.set("{:.3f}".format(mavg))
        self.sim_lbl_trend.set({1: "SUBINDO", -1: "DESCENDO", 0: "ESTAVEL"}[trend])
        self.sim_lbl_alarm.set("ATIVO" if alarm else "ok")
        self._render_cells(self.sim_canvas, self.sim_cells, colmask, trend, int(alarm), center)

    # ---------------- helpers de cor ----------------
    def pick_color(self, i):
        c = colorchooser.askcolor(color=self.col_colors[i], title="Cor da coluna {}".format(i + 1))
        if c and c[1]:
            self.col_colors[i] = c[1]
            self.color_btns[i].config(bg=c[1])

    def pick_alert_color(self):
        c = colorchooser.askcolor(color=self.alert_color, title="Cor do alerta")
        if c and c[1]:
            self.alert_color = c[1]
            self.alert_btn.config(bg=c[1])

    def pick_center_color(self):
        c = colorchooser.askcolor(color=self.center_color, title="Cor do LED central")
        if c and c[1]:
            self.center_color = c[1]
            self.center_btn.config(bg=c[1])

    @staticmethod
    def hex_to_rgb(h):
        h = h.lstrip("#")
        return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)

    # ---------------- interpolacao ----------------
    def interpolate_columns(self):
        """Preenche colunas vazias por interpolacao linear entre as preenchidas."""
        vals = []
        for v in self.col_lambda_vars:
            s = v.get().strip().replace(",", ".")
            vals.append(float(s) if s else None)
        known = [i for i, x in enumerate(vals) if x is not None]
        if len(known) < 2:
            messagebox.showwarning("Interpolar", "Preencha pelo menos 2 colunas.")
            return
        # antes do primeiro / depois do ultimo: mantem o mais proximo
        for i in range(known[0]):
            vals[i] = vals[known[0]]
        for i in range(known[-1] + 1, NUM_COLS):
            vals[i] = vals[known[-1]]
        # entre pontos conhecidos: reta
        for a, b in zip(known, known[1:]):
            for i in range(a + 1, b):
                t = (i - a) / (b - a)
                vals[i] = vals[a] + t * (vals[b] - vals[a])
        for v, x in zip(self.col_lambda_vars, vals):
            v.set("{:.2f}".format(x))

    # ---------------- rows bitmask ----------------
    def rows_mask(self, key):
        m = 0
        for y, v in enumerate(self.rows_vars[key]):
            if v.get():
                m |= (1 << y)
        return m

    # ---------------- mapeamento / teste ----------------
    def send_map(self):
        """Envia o mapeamento ao vivo (sem gravar), p/ ajuste imediato."""
        if self.link.is_open():
            self.link.send("MAP {} {} {} {}".format(
                self.map_serp.get(), self.map_flipx.get(),
                self.map_flipy.get(), self.map_transp.get()))

    def toggle_test(self):
        if self.link.is_open():
            self.link.send("TEST {}".format(self.test_on.get()))

    # ---------------- envio ----------------
    def _f(self, var):
        return float(var.get().strip().replace(",", "."))

    def send_all_static(self):
        if not self.link.is_open():
            messagebox.showwarning("Serial", "Conecte ao Arduino primeiro.")
            return
        try:
            for i in range(NUM_COLS):
                r, g, b = self.hex_to_rgb(self.col_colors[i])
                self.link.send("COLOR {} {} {} {}".format(i, r, g, b))
            ar, ag, ab = self.hex_to_rgb(self.alert_color)
            atype = 1 if self.alert_type.get() == "Piscando" else 0
            self.link.send("ALERT {} {} {}".format(int(self._f(self.alert_time)), atype,
                                                    int(self._f(self.alert_hold))))
            self.link.send("ALERTCOLOR {} {} {}".format(ar, ag, ab))
            self.link.send("CALIB {} {} {} {}".format(self._f(self.v1), self._f(self.l1),
                                                      self._f(self.v2), self._f(self.l2)))
            self.link.send("TRENDROWS {} {} {}".format(self.rows_mask("rise"),
                                                       self.rows_mask("fall"),
                                                       self.rows_mask("stab")))
            self.link.send("STABLE {}".format(self._f(self.stable_thresh)))
            self.link.send("MAVG {}".format(int(self._f(self.mavg_time))))
            self.link.send("SRATE {}".format(int(self._f(self.sample_rate))))
            self.link.send("BRIGHT {}".format(int(self.brightness.get())))
            self.link.send("DIESEL {}".format(self.diesel_mode.get()))
            cr, cg, cb = self.hex_to_rgb(self.center_color)
            self.link.send("CENTER {} {} {} {}".format(self.center_enable.get(), cr, cg, cb))
            self.link.send("MAP {} {} {} {}".format(self.map_serp.get(), self.map_flipx.get(),
                                                    self.map_flipy.get(), self.map_transp.get()))
            self.link.send("SAVE")
            self._log(">> Configuracoes estaticas enviadas e gravadas.")
        except ValueError:
            messagebox.showerror("Valores", "Verifique os campos numericos.")

    def send_fast(self):
        if not self.link.is_open():
            messagebox.showwarning("Serial", "Conecte ao Arduino primeiro.")
            return
        try:
            for i, v in enumerate(self.col_lambda_vars):
                s = v.get().strip().replace(",", ".")
                if s:
                    self.link.send("COLLAMBDA {} {}".format(i, float(s)))
            self.link.send("ALARM {}".format(self._f(self.alarm_lambda)))
            self.link.send("SAVE")
            self._log(">> Ajustes rapidos enviados e gravados.")
        except ValueError:
            messagebox.showerror("Valores", "Verifique os campos numericos.")

    # ---------------- recepcao ----------------
    def _poll_rx(self):
        try:
            while True:
                line = self.rx.get_nowait()
                self._handle_line(line)
        except queue.Empty:
            pass
        self.after(60, self._poll_rx)

    def _handle_line(self, line):
        if line.startswith("D "):
            p = line.split()
            if len(p) >= 8:
                volt, lam, mavg = p[1], p[2], p[3]
                trend = int(p[4])
                colmask, alarm, center = int(p[5]), int(p[6]), int(p[7])
                self.lbl_volt.set(volt)
                self.lbl_lam.set(lam)
                self.lbl_mavg.set(mavg)
                self.lbl_trend.set({1: "SUBINDO", -1: "DESCENDO", 0: "ESTAVEL"}[trend])
                self._update_preview(colmask, trend, alarm, center)
            return
        if line.startswith("CFG"):
            self._parse_cfg(line)
        self._log("<< " + line)

    def _parse_cfg(self, line):
        """Le o dump 'CFG ...' e preenche os campos da interface."""
        toks = line.split()
        for t in toks[1:]:
            if "=" not in t:
                continue
            k, val = t.split("=", 1)
            try:
                if k.startswith("C") and k[1:].isdigit():
                    i = int(k[1:])
                    r, g, b, lam = val.split(",")
                    self.col_colors[i] = "#{:02x}{:02x}{:02x}".format(int(r), int(g), int(b))
                    self.color_btns[i].config(bg=self.col_colors[i])
                    self.col_lambda_vars[i].set("{:.2f}".format(float(lam)))
                elif k == "ALERT":
                    parts = val.split(",")
                    t_, ty, r, g, b = parts[:5]
                    self.alert_time.set(t_)
                    self.alert_type.set("Piscando" if ty == "1" else "Estatico")
                    self.alert_color = "#{:02x}{:02x}{:02x}".format(int(r), int(g), int(b))
                    self.alert_btn.config(bg=self.alert_color)
                    if len(parts) >= 6:
                        self.alert_hold.set(parts[5])
                elif k == "CAL":
                    v1, l1, v2, l2 = val.split(",")
                    self.v1.set(v1); self.l1.set(l1); self.v2.set(v2); self.l2.set(l2)
                elif k == "ROWS":
                    rr, ff, ss = (int(x) for x in val.split(","))
                    for y in range(NUM_ROWS):
                        self.rows_vars["rise"][y].set(1 if rr & (1 << y) else 0)
                        self.rows_vars["fall"][y].set(1 if ff & (1 << y) else 0)
                        self.rows_vars["stab"][y].set(1 if ss & (1 << y) else 0)
                elif k == "STAB":
                    self.stable_thresh.set(val)
                elif k == "MAVG":
                    self.mavg_time.set(val)
                elif k == "SRATE":
                    self.sample_rate.set(val)
                elif k == "ALARM":
                    self.alarm_lambda.set(val)
                elif k == "BRIGHT":
                    self.brightness.set(int(val))
                elif k == "DIESEL":
                    self.diesel_mode.set(int(val))
                elif k == "CENTER":
                    en, r, g, b = val.split(",")
                    self.center_enable.set(int(en))
                    self.center_color = "#{:02x}{:02x}{:02x}".format(int(r), int(g), int(b))
                    self.center_btn.config(bg=self.center_color)
                elif k == "MAP":
                    s, fx, fy, tr = (int(x) for x in val.split(","))
                    self.map_serp.set(s); self.map_flipx.set(fx)
                    self.map_flipy.set(fy); self.map_transp.set(tr)
            except Exception:
                pass
        self._log(">> Configuracao lida do Arduino.")

    def _render_cells(self, canvas, cells, colmask, trend, alarm, center):
        """Pinta uma matriz 8x8 (canvas + cells) com a mesma logica do firmware."""
        rows = self.rows_mask({1: "rise", -1: "fall", 0: "stab"}[trend])
        # quadrado verde central 2x2 quando em repouso
        center_cells = {(3, 3), (3, 4), (4, 3), (4, 4)} if center else set()
        for y in range(8):
            for x in range(8):
                lit = bool(colmask & (1 << x)) and (rows & (1 << y))
                if lit:
                    color = self.col_colors[x]
                elif (x, y) in center_cells:
                    color = self.center_color
                elif alarm:
                    color = self.alert_color
                else:
                    color = "#1a1a1a"
                canvas.itemconfig(cells[y][x], fill=color)

    def _update_preview(self, colmask, trend, alarm, center):
        self._render_cells(self.canvas, self.cells, colmask, trend, alarm, center)

    def _log(self, txt):
        self.logbox.insert("end", txt + "\n")
        self.logbox.see("end")

    def _on_close(self):
        self.link.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
