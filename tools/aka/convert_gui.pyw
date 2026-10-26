#!/usr/bin/env python3
"""Interface simple : choisir des ROMs, cliquer sur Convertir, recuperer des .zip pour la carte SD de l'AKA.

Double-clic sur convert_gui.bat (Windows) ou : python3 convert_gui.pyw
Sous Windows, il faut MSYS2 (https://www.msys2.org, installe dans C:\\msys64) ou WSL :
    pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \\
                       mingw-w64-x86_64-zlib mingw-w64-x86_64-python
    et PlatformIO installe cote Windows (pip install platformio).
"""
import os
import re
import shutil
import subprocess
import sys
import threading

ICI = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(ICI, "convert.sh")
EXT = (".gb", ".gbc", ".sg", ".gg", ".sms", ".zip")


def _abs(p):
    p = p.replace("\\", "/")
    return p if re.match(r"^[A-Za-z]:/", p) else os.path.abspath(p).replace("\\", "/")


def msys_path(p):
    """C:\\a\\b -> /c/a/b"""
    p = _abs(p)
    m = re.match(r"^([A-Za-z]):/(.*)$", p)
    return "/%s/%s" % (m.group(1).lower(), m.group(2)) if m else p


def wsl_path(p):
    p = _abs(p)
    m = re.match(r"^([A-Za-z]):/(.*)$", p)
    return "/mnt/%s/%s" % (m.group(1).lower(), m.group(2)) if m else p


def find_msys2():
    cands = [os.environ.get("MSYS2_ROOT", ""), r"C:\msys64", r"D:\msys64", r"C:\msys32"]
    for root in cands:
        if root and os.path.isfile(os.path.join(root, "usr", "bin", "bash.exe")):
            return root
    return None


def build_command(roms, out_dir, rapide, windows=None):
    """Retourne (commande, environnement) pour lancer convert.sh, ou (None, message d'erreur)."""
    windows = (os.name == "nt") if windows is None else windows
    args = []
    if rapide:
        args.append("-r")
    if not windows:
        return ["bash", SCRIPT, "-o", out_dir] + args + list(roms), None
    msys = find_msys2()
    if msys:
        env = dict(os.environ, MSYSTEM="MINGW64", MSYS2_PATH_TYPE="inherit", CHERE_INVOKING="1")
        conv = [msys_path(p) for p in roms]
        cmd = [os.path.join(msys, "usr", "bin", "bash.exe"), "-lc", 'exec bash "$@"', "bash",
               msys_path(SCRIPT), "-o", msys_path(out_dir)] + args + conv
        return cmd, env
    if shutil.which("wsl"):
        cmd = ["wsl", "bash", wsl_path(SCRIPT), "-o", wsl_path(out_dir)] + args + [wsl_path(p) for p in roms]
        return cmd, None
    return None, ("MSYS2 (C:\\msys64) ou WSL est requis.\n"
                  "Installe MSYS2 depuis https://www.msys2.org puis, dans « MSYS2 MINGW64 » :\n"
                  "pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja "
                  "mingw-w64-x86_64-zlib mingw-w64-x86_64-python\n"
                  "et installe PlatformIO cote Windows : pip install platformio")


def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext

    root = tk.Tk()
    root.title("Convertir une ROM pour la Gamebuino AKA")
    root.geometry("720x520")
    roms = []
    out_var = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "aka-out"))
    fast = tk.BooleanVar(value=False)

    frame = tk.Frame(root, padx=10, pady=10)
    frame.pack(fill="both", expand=True)
    tk.Label(frame, text="ROMs (Game Boy, SG-1000, Game Gear, Master System) :", anchor="w").pack(fill="x")
    lst = tk.Listbox(frame, height=6)
    lst.pack(fill="x")

    def refresh():
        lst.delete(0, "end")
        for r in roms:
            lst.insert("end", r)

    def add_files():
        for f in filedialog.askopenfilenames(title="Choisir des ROMs",
                filetypes=[("ROMs", "*.gb *.gbc *.sg *.gg *.sms *.zip"), ("Tous", "*.*")]):
            if f not in roms:
                roms.append(f)
        refresh()

    def add_folder():
        d = filedialog.askdirectory(title="Dossier de ROMs")
        if d and d not in roms:
            roms.append(d)
        refresh()

    def remove():
        for i in reversed(lst.curselection()):
            del roms[i]
        refresh()

    def choose_out():
        d = filedialog.askdirectory(title="Dossier de sortie")
        if d:
            out_var.set(d)

    bar = tk.Frame(frame)
    bar.pack(fill="x", pady=4)
    tk.Button(bar, text="Ajouter des ROMs…", command=add_files).pack(side="left")
    tk.Button(bar, text="Ajouter un dossier…", command=add_folder).pack(side="left", padx=4)
    tk.Button(bar, text="Retirer", command=remove).pack(side="left")

    row = tk.Frame(frame)
    row.pack(fill="x", pady=4)
    tk.Label(row, text="Sortie :").pack(side="left")
    tk.Entry(row, textvariable=out_var).pack(side="left", fill="x", expand=True, padx=4)
    tk.Button(row, text="…", command=choose_out).pack(side="left")
    tk.Checkbutton(frame, text="Conversion rapide (couverture courte, pour essayer)", variable=fast).pack(anchor="w")

    log = scrolledtext.ScrolledText(frame, height=14, state="disabled", font=("Consolas", 9))
    log.pack(fill="both", expand=True, pady=4)

    def write(text):
        log.configure(state="normal")
        log.insert("end", text)
        log.see("end")
        log.configure(state="disabled")

    btn = tk.Button(frame, text="Convertir", font=("", 11, "bold"))
    btn.pack(fill="x")

    def worker(cmd, env, out):
        try:
            p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for raw in iter(p.stdout.readline, b""):
                root.after(0, write, raw.decode("utf-8", "replace"))
            code = p.wait()
        except OSError as e:
            code = -1
            root.after(0, write, "Erreur : %s\n" % e)
        zips = [f for f in os.listdir(out) if f.lower().endswith(".zip")] if os.path.isdir(out) else []
        root.after(0, write, "\n== Termine : %d jeu(x) dans %s ==\n" % (len(zips), out))
        root.after(0, lambda: btn.configure(state="normal"))
        if zips and os.name == "nt":
            root.after(0, lambda: os.startfile(out))

    def go():
        if not roms:
            messagebox.showinfo("Convertir", "Ajoute au moins une ROM.")
            return
        out = out_var.get()
        os.makedirs(out, exist_ok=True)
        cmd, env = build_command(roms, out, fast.get())
        if cmd is None:
            messagebox.showerror("Outils manquants", env)
            return
        btn.configure(state="disabled")
        write("> " + " ".join(cmd[:4]) + " ...\n")
        threading.Thread(target=worker, args=(cmd, env, out), daemon=True).start()

    btn.configure(command=go)
    root.mainloop()


if __name__ == "__main__":
    run_gui()
