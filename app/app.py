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
import tkinter as tk
from tkinter import ttk, colorchooser, messagebox

import serial
import serial.tools.list_ports

BAUD = 115200
NUM_COLS = 8
NUM_ROWS = 8

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
        self.tab_cfg = ttk.Frame(nb)
        self.tab_fast = ttk.Frame(nb)
        nb.add(self.tab_cfg, text="  Configuracoes  ")
        nb.add(self.tab_fast, text="  Rapido  ")
        self._build_cfg_tab()
        self._build_fast_tab()

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
        f = self.tab_cfg

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
        ttk.Label(g2, text="Tempo/periodo (ms):").grid(row=0, column=0, sticky="e", padx=4, pady=4)
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

        # Linhas por tendencia
        g4 = ttk.LabelFrame(f, text="Linhas acesas por tendencia da media movel  (1=topo ... 8=base)")
        g4.pack(fill="x", padx=8, pady=6)
        labels = [("Subindo", "rise"), ("Descendo", "fall"), ("Estavel", "stab")]
        for r, (txt, key) in enumerate(labels):
            ttk.Label(g4, text=txt + ":").grid(row=r, column=0, sticky="e", padx=4, pady=3)
            for y in range(NUM_ROWS):
                v = tk.IntVar()
                ttk.Checkbutton(g4, variable=v).grid(row=r, column=1 + y, padx=1)
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

        ttk.Button(f, text="Enviar e Gravar configuracoes (SAVE)",
                   command=self.send_all_static).pack(pady=10)

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
        ttk.Label(g2, text="Lambda que dispara o alerta:").grid(row=0, column=0, sticky="e", padx=4, pady=6)
        self.alarm_lambda = tk.StringVar(value="1.40")
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
            self.link.send("ALERT {} {}".format(int(self._f(self.alert_time)), atype))
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
            if len(p) >= 7:
                volt, lam, mavg = p[1], p[2], p[3]
                trend, cols, alarm = int(p[4]), int(p[5]), int(p[6])
                self.lbl_volt.set(volt)
                self.lbl_lam.set(lam)
                self.lbl_mavg.set(mavg)
                self.lbl_trend.set({1: "SUBINDO", -1: "DESCENDO", 0: "ESTAVEL"}[trend])
                self._update_preview(cols, trend, alarm)
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
                    t_, ty, r, g, b = val.split(",")
                    self.alert_time.set(t_)
                    self.alert_type.set("Piscando" if ty == "1" else "Estatico")
                    self.alert_color = "#{:02x}{:02x}{:02x}".format(int(r), int(g), int(b))
                    self.alert_btn.config(bg=self.alert_color)
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
            except Exception:
                pass
        self._log(">> Configuracao lida do Arduino.")

    def _update_preview(self, cols, trend, alarm):
        rows = self.rows_mask({1: "rise", -1: "fall", 0: "stab"}[trend])
        for y in range(8):
            for x in range(8):
                lit = (x < cols) and (rows & (1 << y))
                color = self.col_colors[x] if lit else "#1a1a1a"
                if not lit and alarm:
                    color = self.alert_color
                self.canvas.itemconfig(self.cells[y][x], fill=color)

    def _log(self, txt):
        self.logbox.insert("end", txt + "\n")
        self.logbox.see("end")

    def _on_close(self):
        self.link.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
