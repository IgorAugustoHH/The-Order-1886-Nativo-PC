#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TOport: launcher for The Order: 1886 on Windows (Tkinter).

The player picks the folder of their own dump (CUSA00785, version 01.02) and the settings the
in-game menu (Insert / L3+R3) also edits; they live in bbport.ini next to this program, and
run.py turns them into the port's environment. Launcher-only options (display mode, game
folder) are in toport.json in the same folder.

Frozen with PyInstaller (packaging/windows/package_toport.sh) the same TOport.exe also runs the
game without the window (`--play`), run.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import threading

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
CONFIG = DATA_DIR / 'bbport.ini'
LAUNCHER_FILE = DATA_DIR / 'toport.json'
LOG_FILE = DATA_DIR / 'last_run.log'
TITLE_ID = 'CUSA00785'
GAME_VERSION = '01.02'
VERSION = '0.1'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable (as the BBPort launcher).

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    """The command that starts run.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Settings: bbport.ini (shared with the in-game menu) and toport.json (launcher only).

AA_MODES = [('game', 'TAA original do jogo'), ('dlaa', 'DLSS DLAA (resolução nativa)'),
            ('quality', 'DLSS Qualidade'), ('balanced', 'DLSS Equilibrado'),
            ('performance', 'DLSS Desempenho'), ('ultra', 'DLSS Ultra Desempenho')]
RESOLUTIONS = [('720', '1280 x 720 (padrão do jogo)'), ('1080', '1920 x 1080')]
FPS_LIMITS = [('30', '30 FPS'), ('60', '60 FPS'), ('90', '90 FPS'), ('120', '120 FPS')]
# PS4 system language codes (gpu/shim/bbport_settings.h GameLanguages).
LANGUAGES = [('17', 'Português (Brasil)'), ('7', 'Português (Portugal)'), ('1', 'English (US)'),
             ('18', 'English (UK)'), ('3', 'Español (España)'), ('20', 'Español (Latinoamérica)'),
             ('2', 'Français'), ('22', 'Français (Canada)'), ('4', 'Deutsch'), ('5', 'Italiano'),
             ('6', 'Nederlands'), ('8', 'Русский'), ('16', 'Polski'), ('19', 'Türkçe'),
             ('12', 'Suomi'), ('13', 'Svenska'), ('14', 'Dansk'), ('15', 'Norsk'),
             ('21', 'العربية (Arabic)'), ('0', '日本語 (Japanese)'), ('9', '한국어 (Korean)'),
             ('10', '中文 繁體 (Traditional Chinese)'), ('11', '中文 简体 (Simplified Chinese)')]
DISPLAY_MODES = [('1', 'Tela cheia sem bordas'), ('2', 'Tela cheia exclusiva'), ('0', 'Janela')]
INI_DEFAULTS = {'order_aa': 'quality', 'order_resolution': '1080', 'fps_limit': '60',
                'game_language': '17', 'photo_camera': '1', 'fast_loading': '1', 'show_fps': '1'}


def read_ini():
    values = dict(INI_DEFAULTS)
    try:
        for line in CONFIG.read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and not key.startswith('#'):
                values[key] = value
    except OSError:
        pass
    return values


def write_ini(changes):
    """Updates the given keys, keeping every other line (run.py writes the defaults)."""
    try:
        lines = CONFIG.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        lines = ['# bbport settings (in-game menu: Insert / L3+R3)']
    pending = dict(changes)
    for i, line in enumerate(lines):
        key = line.partition('=')[0].strip()
        if key in pending:
            lines[i] = f'{key}={pending.pop(key)}'
    lines += [f'{key}={value}' for key, value in pending.items()]
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def read_launcher():
    try:
        return json.loads(LAUNCHER_FILE.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {'game_dir': str(PORT_DIR.parent / TITLE_ID), 'display': '1'}


def write_launcher(values):
    LAUNCHER_FILE.write_text(json.dumps(values, indent=1, ensure_ascii=False), encoding='utf-8')


def read_sfo(path):
    """param.sfo key/value pairs (TITLE_ID, APP_VER, TITLE...)."""
    data = path.read_bytes()
    if data[:4] != b'\0PSF':
        raise ValueError('param.sfo inválido')
    key_table, data_table, count = struct.unpack_from('<III', data, 8)
    values = {}
    for i in range(count):
        key_off, fmt, length, _, data_off = struct.unpack_from('<HHIII', data, 20 + i * 16)
        key = data[key_table + key_off:data.index(b'\0', key_table + key_off)].decode()
        raw = data[data_table + data_off:data_table + data_off + length]
        values[key] = struct.unpack('<I', raw[:4])[0] if fmt == 0x0404 else \
            raw.split(b'\0')[0].decode('utf-8', 'replace')
    return values


def check_game(folder):
    """(ok, message) for a game folder: the port is verified on CUSA00785 01.02 only."""
    folder = Path(folder)
    if not (folder / 'eboot.bin').is_file():
        return False, 'Esta pasta não tem eboot.bin. Escolha a pasta do seu dump (CUSA00785).'
    try:
        sfo = read_sfo(folder / 'sce_sys' / 'param.sfo')
    except (OSError, ValueError) as error:
        return False, f'Não foi possível ler sce_sys/param.sfo ({error}).'
    title, version = sfo.get('TITLE_ID'), sfo.get('APP_VER')
    if title != TITLE_ID:
        return False, f'Este dump é {title}; o TOport roda The Order: 1886 ({TITLE_ID}).'
    if version != GAME_VERSION:
        return False, (f'Versão {version}: o TOport foi verificado na {GAME_VERSION} '
                       '(as correções do executável recusam outras versões).')
    return True, f'The Order: 1886 · {TITLE_ID} · versão {version}'


def game_environment(launcher):
    env = dict(os.environ)
    env['BB_GAME_DIR'] = launcher['game_dir']
    env['BB_DATA_DIR'] = str(DATA_DIR)
    env['BB_CONFIG'] = str(CONFIG)
    env['BB_FULLSCREEN'] = launcher.get('display', '1')
    return env


def play_without_window():
    """--play: starts the game with the saved settings and waits for it (shortcuts, Steam)."""
    launcher = read_launcher()
    ok, message = check_game(launcher['game_dir'])
    if not ok:
        ctypes.windll.user32.MessageBoxW(None, message, 'TOport', 0x10)
        return 1
    with open(LOG_FILE, 'w', encoding='utf-8', errors='replace') as log:
        return subprocess.run(run_command(), cwd=PORT_DIR, env=game_environment(launcher),
                              stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                              creationflags=NO_WINDOW).returncode


# ---------------------------------------------------------------------------------------------
# Window.

def main_window():
    import tkinter as tk
    from tkinter import filedialog, ttk

    root = tk.Tk()
    root.title(f'TOport {VERSION} — The Order: 1886')
    root.resizable(False, False)
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    style = ttk.Style(root)
    style.theme_use('clam')
    background, panel, text, accent = '#16130f', '#221d17', '#e8dcc6', '#b08d57'
    root.configure(bg=background)
    style.configure('.', background=background, foreground=text, fieldbackground=panel)
    style.configure('TLabel', background=background, foreground=text, font=('Segoe UI', 10))
    style.configure('Title.TLabel', font=('Georgia', 20, 'bold'), foreground=accent)
    style.configure('Small.TLabel', font=('Segoe UI', 9), foreground='#a89a83')
    style.configure('TLabelframe', background=background, foreground=accent)
    style.configure('TLabelframe.Label', background=background, foreground=accent,
                    font=('Segoe UI', 10, 'bold'))
    style.configure('TCheckbutton', background=background, foreground=text)
    style.map('TCheckbutton', background=[('active', background)])
    style.configure('TButton', background=panel, foreground=text)
    style.configure('Play.TButton', font=('Segoe UI', 12, 'bold'), padding=(24, 8))
    style.map('TCombobox', fieldbackground=[('readonly', panel)], foreground=[('readonly', text)])

    ini = read_ini()
    launcher = read_launcher()
    frame = ttk.Frame(root, padding=18)
    frame.grid()
    ttk.Label(frame, text='THE ORDER: 1886', style='Title.TLabel').grid(columnspan=3, sticky='w')
    ttk.Label(frame, text='TOport — port nativo para PC (base BBPort)',
              style='Small.TLabel').grid(columnspan=3, sticky='w', pady=(0, 12))

    # Game folder.
    game = ttk.LabelFrame(frame, text='Jogo', padding=10)
    game.grid(columnspan=3, sticky='we', pady=4)
    game_dir = tk.StringVar(value=launcher.get('game_dir', ''))
    status = tk.StringVar()
    entry = ttk.Entry(game, textvariable=game_dir, width=60)
    entry.grid(row=0, column=0, sticky='we')

    def refresh_status(*_):
        ok, message = check_game(game_dir.get())
        status.set(('✔ ' if ok else '✖ ') + message)

    def browse():
        folder = filedialog.askdirectory(title='Pasta do seu dump de The Order: 1886 (CUSA00785)',
                                         initialdir=game_dir.get() or str(PORT_DIR))
        if folder:
            game_dir.set(folder)

    ttk.Button(game, text='Procurar…', command=browse).grid(row=0, column=1, padx=(6, 0))
    ttk.Label(game, textvariable=status, style='Small.TLabel', wraplength=520).grid(
        row=1, columnspan=2, sticky='w', pady=(6, 0))
    game_dir.trace_add('write', refresh_status)
    refresh_status()

    # Settings (the same as the in-game menu).
    combos = {}

    def combo(parent, row, label, key, options, value):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky='w', pady=3)
        names = [name for _, name in options]
        current = next((name for code, name in options if code == value), names[0])
        var = tk.StringVar(value=current)
        box = ttk.Combobox(parent, textvariable=var, values=names, state='readonly', width=34)
        box.grid(row=row, column=1, sticky='w', padx=(10, 0), pady=3)
        combos[key] = (var, options)

    checks = {}

    def check(parent, row, label, key):
        var = tk.BooleanVar(value=ini.get(key, '0') == '1')
        ttk.Checkbutton(parent, text=label, variable=var).grid(row=row, columnspan=2,
                                                                sticky='w', pady=2)
        checks[key] = var

    image = ttk.LabelFrame(frame, text='Imagem e desempenho', padding=10)
    image.grid(columnspan=3, sticky='we', pady=4)
    combo(image, 0, 'Anti-aliasing', 'order_aa', AA_MODES, ini['order_aa'])
    combo(image, 1, 'Resolução do jogo', 'order_resolution', RESOLUTIONS, ini['order_resolution'])
    combo(image, 2, 'Limite de quadros (F9 no jogo)', 'fps_limit', FPS_LIMITS, ini['fps_limit'])
    combo(image, 3, 'Modo de tela', 'display', DISPLAY_MODES, launcher.get('display', '1'))
    check(image, 4, 'Carregamento acelerado (recursos e iluminação chegam mais rápido)',
          'fast_loading')
    check(image, 5, 'Mostrar FPS no canto', 'show_fps')

    options = ttk.LabelFrame(frame, text='Jogo e idioma', padding=10)
    options.grid(columnspan=3, sticky='we', pady=4)
    combo(options, 0, 'Idioma do jogo', 'game_language', LANGUAGES, ini['game_language'])
    check(options, 1, 'Câmera livre nas cutscenes (modo foto)', 'photo_camera')

    ttk.Label(frame, text='No jogo: Insert ou L3+R3 abre o menu com estas mesmas opções.',
              style='Small.TLabel').grid(columnspan=3, sticky='w', pady=(6, 0))

    message = tk.StringVar()
    ttk.Label(frame, textvariable=message, style='Small.TLabel', wraplength=560).grid(
        columnspan=3, sticky='w', pady=(4, 0))

    def save():
        changes = {}
        for key, (var, opts) in combos.items():
            code = next(c for c, name in opts if name == var.get())
            if key == 'display':
                launcher['display'] = code
            else:
                changes[key] = code
        for key, var in checks.items():
            changes[key] = '1' if var.get() else '0'
        write_ini(changes)
        launcher['game_dir'] = game_dir.get()
        write_launcher(launcher)

    def play():
        save()
        ok, text_ = check_game(game_dir.get())
        if not ok:
            message.set(text_)
            return
        button.state(['disabled'])
        message.set('Preparando e iniciando o jogo… (a primeira vez demora mais: o jogo é '
                    'preparado e os shaders são compilados enquanto você joga)')

        def worker():
            with open(LOG_FILE, 'w', encoding='utf-8', errors='replace') as log:
                code = subprocess.run(run_command(), cwd=PORT_DIR, env=game_environment(launcher),
                                      stdin=subprocess.DEVNULL, stdout=log,
                                      stderr=subprocess.STDOUT, creationflags=NO_WINDOW).returncode

            def done():
                button.state(['!disabled'])
                message.set('Jogo encerrado.' if code == 0 else
                            f'O jogo terminou com o código {code:#x}. Registro: {LOG_FILE}')
            root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    buttons = ttk.Frame(frame)
    buttons.grid(columnspan=3, sticky='we', pady=(12, 0))
    button = ttk.Button(buttons, text='JOGAR', style='Play.TButton', command=play)
    button.pack(side='right')
    ttk.Button(buttons, text='Salvar', command=lambda: (save(), message.set('Configurações salvas.'))
               ).pack(side='right', padx=6)
    ttk.Button(buttons, text='Abrir pasta do TOport',
               command=lambda: os.startfile(DATA_DIR)).pack(side='left')
    root.protocol('WM_DELETE_WINDOW', lambda: (save(), root.destroy()))
    root.mainloop()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] in ('--run', '--script'):
        sys.exit(run_role(sys.argv[1:]))
    if len(sys.argv) > 1 and sys.argv[1] == '--play':
        sys.exit(play_without_window())
    main_window()
