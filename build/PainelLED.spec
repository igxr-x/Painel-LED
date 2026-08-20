# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['D:/Projetos/Claude/PAINEL LED/app/app.py'],
    pathex=[],
    binaries=[],
    datas=[('D:/Projetos/Claude/PAINEL LED/app/LOGO.png', '.'), ('D:/Projetos/Claude/PAINEL LED/app/LOGO_header.png', '.'), ('D:/Projetos/Claude/PAINEL LED/app/app.ico', '.')],
    hiddenimports=['serial.tools.list_ports'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['PIL', 'numpy'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='PainelLED',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['D:/Projetos/Claude/PAINEL LED/app/app.ico'],
)
