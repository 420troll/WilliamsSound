#!/usr/bin/env python3
"""Fire! virtual ESP32 sound-command monitor for Windows (Python standard library).

Watches PinMAME's fire_s11_bus_trace.csv and treats a 0->1 CA2/CB2
transition as a candidate gameplay sound command. Uses *confirmed* F4
board/byte mappings to play recordings. Does not emulate electrical timing.
"""
import csv
import os
from pathlib import Path
import sys
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

if sys.platform != 'win32':
    raise SystemExit('This audio version requires Windows (winsound).')
import winsound

ROOT = Path(r'C:\VPinMAME')
RUN = ROOT / 'fire_trace_build' / 'Release'
DEFAULT_TRACE = RUN / 'fire_s11_bus_trace.csv'
DEFAULT_MAP = RUN / 'fire_f4_verified_mapping.csv'
DEFAULT_WAV = ROOT / 'pinmame' / 'fire_audio'


def normalize_board(value):
    s = str(value).strip().upper().replace(' ', '')
    return {'0': 'BOARD0', '1': 'BOARD1', 'CPU_SOUND': 'BOARD0',
            'EXTERNAL': 'BOARD1'}.get(s, s)


def norm_hex(value):
    try:
        return f'{int(str(value).strip(), 16) & 255:02X}'
    except (ValueError, TypeError):
        return ''


class App:
    def __init__(self, root):
        self.root = root
        self.root.title('Fire! — Virtual ESP32 Sound Emulator v1')
        self.root.geometry('1100x720')
        self.trace_path = tk.StringVar(value=str(DEFAULT_TRACE))
        self.map_path = tk.StringVar(value=str(DEFAULT_MAP))
        self.wav_dir = tk.StringVar(value=str(DEFAULT_WAV))
        self.enabled = tk.BooleanVar(value=False)
        self.play_enabled = tk.BooleanVar(value=True)
        self.fallback = tk.BooleanVar(value=False)
        self.board_sel = tk.StringVar(value='BOARD0')
        self.byte_sel = tk.StringVar(value='CB')
        self.status = tk.StringVar(value='Load mappings, then click START MONITOR.')
        self.counts = {'BOARD0': 0, 'BOARD1': 0}
        self.count_labels = {}
        self.mappings = {}
        self.last_data = {}
        self.last_strobe = {}
        self.offset = 0
        self.fragment = ''
        self.file_id = None
        self.last_mtime = 0
        self.pending = []
        self._build()
        self.load_map()
        self.root.after(160, self.poll)
        self.root.protocol('WM_DELETE_WINDOW', self.close)

    def _build(self):
        top = ttk.Frame(self.root, padding=10)
        top.pack(fill='x')
        for row, (label, var, callback) in enumerate([
            ('Trace CSV', self.trace_path, self.browse_trace),
            ('Verified map CSV', self.map_path, self.browse_map),
            ('WAV directory', self.wav_dir, self.browse_wavs),
        ]):
            ttk.Label(top, text=label, width=18).grid(row=row, column=0, sticky='w', pady=3)
            ttk.Entry(top, textvariable=var).grid(row=row, column=1, sticky='ew', padx=5)
            ttk.Button(top, text='Browse', command=callback).grid(row=row, column=2)
        top.columnconfigure(1, weight=1)
        buttons = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        buttons.pack(fill='x')
        self.run_btn = ttk.Button(buttons, text='START MONITOR', command=self.toggle)
        self.run_btn.pack(side='left', padx=(0, 8))
        ttk.Button(buttons, text='Reload mappings', command=self.load_map).pack(side='left', padx=4)
        ttk.Button(buttons, text='Stop audio', command=self.stop_audio).pack(side='left', padx=4)
        ttk.Button(buttons, text='Export events', command=self.export).pack(side='left', padx=4)
        ttk.Button(buttons, text='Clear events', command=self.clear).pack(side='left', padx=4)
        ttk.Checkbutton(buttons, text='Play mapped WAVs', variable=self.play_enabled).pack(side='left', padx=12)
        ttk.Checkbutton(buttons, text='Try unverified filename match (experimental)', variable=self.fallback).pack(side='left')
        indicator = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        indicator.pack(fill='x')
        for name in ('BOARD0', 'BOARD1'):
            pane = ttk.LabelFrame(indicator, text=f'{name}  /  {"CPU_SOUND" if name == "BOARD0" else "EXTERNAL"}', padding=12)
            pane.pack(side='left', fill='x', expand=True, padx=4)
            v = tk.StringVar(value='0 received')
            ttk.Label(pane, textvariable=v, font=('Segoe UI', 14, 'bold')).pack(anchor='w')
            self.count_labels[name] = v
        inject = ttk.LabelFrame(self.root, text='Manual command injection (no PinMAME needed)', padding=8)
        inject.pack(fill='x', padx=10, pady=(0, 8))
        ttk.Label(inject, text='Board').pack(side='left')
        ttk.Combobox(inject, textvariable=self.board_sel, values=['BOARD0', 'BOARD1'], width=12, state='readonly').pack(side='left', padx=6)
        ttk.Label(inject, text='Hex byte').pack(side='left')
        ttk.Entry(inject, textvariable=self.byte_sel, width=8).pack(side='left', padx=6)
        ttk.Button(inject, text='INJECT COMMAND', command=self.inject).pack(side='left', padx=6)
        ttk.Label(inject, text='Matches are board-specific; this simulates the decoded command only.').pack(side='left', padx=10)
        grid = ttk.Frame(self.root, padding=(10, 0, 10, 5))
        grid.pack(fill='both', expand=True)
        cols = ('time', 'origin', 'board', 'byte', 'f4', 'wav', 'result')
        self.table = ttk.Treeview(grid, columns=cols, show='headings', height=16)
        labels = {'time': 'Time (s)', 'origin': 'Source', 'board': 'Board', 'byte': 'Byte',
                  'f4': 'F4 command', 'wav': 'Recording', 'result': 'Result'}
        widths = {'time': 100, 'origin': 95, 'board': 95, 'byte': 65,
                  'f4': 100, 'wav': 380, 'result': 200}
        for c in cols:
            self.table.heading(c, text=labels[c]); self.table.column(c, width=widths[c], stretch=c == 'wav')
        sc = ttk.Scrollbar(grid, command=self.table.yview)
        self.table.configure(yscrollcommand=sc.set)
        self.table.pack(side='left', fill='both', expand=True)
        sc.pack(side='left', fill='y')
        ttk.Label(self.root, textvariable=self.status, padding=10, anchor='w').pack(fill='x')
        ttk.Label(self.root, text='Trace is decoded on CA2/CB2 low-to-high transitions. Mapping is from verified F4 dispatches; real bus equivalence remains unverified.', padding=(10, 0, 10, 12), foreground='#666666', wraplength=1000).pack(fill='x')

    def browse_trace(self):
        p = filedialog.askopenfilename(filetypes=[('CSV', '*.csv'), ('All files', '*.*')])
        if p: self.trace_path.set(p)

    def browse_map(self):
        p = filedialog.askopenfilename(filetypes=[('CSV', '*.csv'), ('All files', '*.*')])
        if p: self.map_path.set(p); self.load_map()

    def browse_wavs(self):
        p = filedialog.askdirectory()
        if p: self.wav_dir.set(p)

    def load_map(self):
        self.mappings = {}
        path = Path(self.map_path.get())
        if not path.is_file():
            self.status.set(f'No verified mappings yet: {path}. Use manual injection or enable experimental filename matching.')
            return
        try:
            with path.open(newline='', encoding='utf-8-sig') as f:
                for row in csv.DictReader(f):
                    if str(row.get('status', '')).lower() != 'confirmed':
                        continue
                    board = normalize_board(row.get('board', ''))
                    byte = norm_hex(row.get('board_byte', ''))
                    if board not in ('BOARD0', 'BOARD1') or not byte:
                        continue
                    self.mappings[(board, byte)] = row
        except (OSError, csv.Error) as exc:
            messagebox.showerror('Mapping error', str(exc))
            return
        self.status.set(f'Loaded {len(self.mappings)} confirmed board/byte mappings from {path.name}.')

    def toggle(self):
        if self.enabled.get():
            self.enabled.set(False)
            self.run_btn.configure(text='START MONITOR')
            self.status.set('Monitor stopped.')
            return
        self.offset = 0
        self.fragment = ''
        self.file_id = None
        self.last_data = {}
        self.last_strobe = {}
        # Ignore preexisting events: start monitoring from current end of file.
        p = Path(self.trace_path.get())
        if p.is_file():
            self.offset = p.stat().st_size
            self.file_id = (p.stat().st_dev, p.stat().st_ino)
        self.enabled.set(True)
        self.run_btn.configure(text='STOP MONITOR')
        self.status.set('Listening for NEW trace events. Start Fire! and operate switches (not F4).')

    def poll(self):
        if self.enabled.get():
            try: self.read_trace()
            except Exception as exc:
                self.status.set(f'Trace read error: {exc}')
        self.root.after(160, self.poll)

    def read_trace(self):
        p = Path(self.trace_path.get())
        if not p.is_file(): return
        st = p.stat()
        identity = (st.st_dev, st.st_ino)
        if self.file_id is not None and (identity != self.file_id or st.st_size < self.offset):
            self.offset = 0; self.fragment = ''; self.last_data.clear(); self.last_strobe.clear()
        self.file_id = identity
        if st.st_size == self.offset: return
        with p.open('rb') as f:
            f.seek(self.offset)
            raw = f.read()
            self.offset = f.tell()
        chunk = self.fragment + raw.decode('utf-8', errors='replace')
        lines = chunk.split('\n')
        self.fragment = lines.pop()
        for line in lines:
            if line.strip(): self.parse_line(line.rstrip('\r'))

    def parse_line(self, line):
        parts = line.split(',', 3)
        if len(parts) != 4: return
        stamp, path, signal, value = (s.strip() for s in parts)
        board = {'CPU_SOUND': 'BOARD0', 'EXTERNAL': 'BOARD1'}.get(path)
        if not board: return
        if signal == 'DATA':
            self.last_data[board] = norm_hex(value)
        elif (board == 'BOARD0' and signal == 'CA2') or (board == 'BOARD1' and signal == 'CB2'):
            v = value.upper()
            old = self.last_strobe.get(board)
            self.last_strobe[board] = v
            if old == '00' and v == '01' and self.last_data.get(board):
                self.on_command(stamp, 'TRACE', board, self.last_data[board])

    def find_wav(self, board, byte):
        mapping = self.mappings.get((board, byte))
        wavfolder = Path(self.wav_dir.get())
        if mapping:
            name = mapping.get('wav_file', '').strip()
            if name:
                p = Path(name)
                target = p if p.is_absolute() else wavfolder / p
                return target, mapping.get('f4_command', ''), True
        if self.fallback.get():
            candidates = sorted(wavfolder.glob(f'{byte} - *.wav'))
            candidates += sorted(wavfolder.glob(f'00{byte} - *.wav'))
            if candidates:
                return candidates[0], '?', False
        return None, '', False

    def on_command(self, stamp, origin, board, byte):
        self.counts[board] += 1
        self.count_labels[board].set(f'{self.counts[board]} received  ·  last {byte}')
        path, f4, verified = self.find_wav(board, byte)
        result = 'No confirmed mapping'
        if path is not None:
            if not path.is_file():
                result = 'WAV missing'
            elif not verified:
                result = 'UNVERIFIED filename guess'
            else:
                result = 'Confirmed F4 mapping'
            if path.is_file() and self.play_enabled.get():
                try:
                    # stdlib winsound supports only a single active playback at a time.
                    winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
                    result += ' · playing'
                except RuntimeError as exc:
                    result = f'Audio error: {exc}'
        self.table.insert('', 'end', values=(stamp, origin, board, byte, f4,
                                             path.name if path else '', result))
        self.table.yview_moveto(1)
        self.status.set(f'{board}:{byte}  —  {result}')

    def inject(self):
        board = normalize_board(self.board_sel.get())
        byte = norm_hex(self.byte_sel.get())
        if board not in ('BOARD0', 'BOARD1') or not byte:
            messagebox.showwarning('Invalid command', 'Select a board and enter an 8-bit hex value (00–FF).')
            return
        self.on_command(f'{time.time():.3f}', 'MANUAL', board, byte)

    def stop_audio(self):
        winsound.PlaySound(None, 0)
        self.status.set('Audio stopped.')

    def clear(self):
        for item in self.table.get_children(): self.table.delete(item)
        self.counts = {'BOARD0': 0, 'BOARD1': 0}
        for v in self.count_labels.values(): v.set('0 received')

    def export(self):
        p = filedialog.asksaveasfilename(initialfile='fire_virtual_esp32_events.csv',
                                         defaultextension='.csv', filetypes=[('CSV', '*.csv')])
        if not p: return
        with open(p, 'w', newline='', encoding='utf-8-sig') as f:
            w = csv.writer(f)
            w.writerow(['timestamp', 'source', 'board', 'byte', 'f4_command', 'wav_file', 'result'])
            for item in self.table.get_children(): w.writerow(self.table.item(item, 'values'))
        self.status.set(f'Exported {len(self.table.get_children())} events to {p}.')

    def close(self):
        winsound.PlaySound(None, 0)
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    App(root)
    root.mainloop()