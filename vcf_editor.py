#!/usr/bin/env python3
"""vcf_editor -- a drag-and-drop GUI for Speechify/RealSpeak .vcf voice configs.

A VCF is a 2:1 NIBBLE cipher, not XOR: every plaintext byte becomes two bytes,
high nibble then low, through a 16-entry table. So it cannot be opened in a
text editor, and the selector weights that decide how a voice actually sounds
(JOIN_COST_WEIGHT, ABS_F0_WEIGHT, CHUNK_BIAS_WEIGHT, ...) are locked inside it.

    py vcf_editor.py                 # drop a .vcf on the window
    py vcf_editor.py voice.vcf       # or open one directly

⛔ TWO RULES THIS ENFORCES, BOTH LEARNED THE EXPENSIVE WAY.

1. YOU CANNOT INVENT PARAMETER NAMES. The real Speechify server validates the
   VCF against a DTD that rejects UNKNOWN names -- not absent ones -- and exits
   rc=5. So there is no free-text "add a parameter" box anywhere in this UI.
   Only names already in the file can be edited, plus the two in
   vcf_variant.ATTESTED_ADDITIONS, each of which is attested by a shipped voice
   or by the engine binary itself.

2. IT DOES NOT OVERWRITE YOUR VOICE. Saving defaults to a NEW file beside the
   original, because spfy_synth takes the VCF path as an argument and a sweep
   is supposed to point at copies while the byte-exact audit keeps running
   against the untouched original. Overwriting in place is possible and asks
   first, every time.

The codec is imported from vcf_variant.py rather than reimplemented -- one
implementation, already used by the shipping build tools.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys
import traceback
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = Path(__file__).resolve().parent

# vcf_variant.py ships beside the voice it was written for and is mirrored in
# the reveng tree; take whichever exists rather than pinning one path.
for cand in (HERE / "en-US" / "crsmara" / "build_tools",
             HERE / "reveng" / "spfy4" / "tools",
             HERE):
    if (cand / "vcf_variant.py").is_file():
        sys.path.insert(0, str(cand))
        break
try:
    import vcf_variant as VV
except ImportError:
    print("cannot find vcf_variant.py -- expected in en-US/crsmara/build_tools/",
          file=sys.stderr)
    raise

try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    HAVE_DND = True
except ImportError:
    HAVE_DND = False


BG = "#1e1e1e"
FG = "#e6e6e6"
ACCENT = "#4ea1ff"
CHANGED = "#3a2f00"
MUTED = "#8a8a8a"


def parse_drop(data):
    """tkdnd hands over a Tcl list: braced when a path contains spaces."""
    out, buf, depth = [], "", 0
    for ch in data:
        if ch == "{":
            depth += 1
            if depth == 1:
                continue
        if ch == "}":
            depth -= 1
            if depth == 0:
                out.append(buf)
                buf = ""
                continue
        if ch == " " and depth == 0:
            if buf:
                out.append(buf)
                buf = ""
            continue
        buf += ch
    if buf:
        out.append(buf)
    return [p for p in out if p]


class Editor:
    def __init__(self, root):
        self.root = root
        self.path = None
        self.orig = {}          # name -> value as loaded
        self.vars = {}          # name -> tk.StringVar for the edit field
        self.rows = {}          # name -> (frame, name_lbl, orig_lbl, entry)
        self.roundtrip = None

        root.title("VCF editor")
        root.geometry("880x620")
        root.configure(bg=BG)
        root.minsize(680, 420)

        self._build_header()
        self._build_table()
        self._build_footer()
        self._wire_dnd()
        self._set_status("Drop a .vcf here, or use Open." if HAVE_DND
                         else "Use Open to choose a .vcf "
                              "(pip install tkinterdnd2 for drag and drop).")

    # ---------------- layout ----------------

    def _build_header(self):
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill="x", padx=12, pady=(12, 6))

        tk.Button(bar, text="Open…", command=self.open_dialog,
                  bg="#2d2d2d", fg=FG, relief="flat",
                  activebackground="#3a3a3a", activeforeground=FG,
                  padx=14, pady=4).pack(side="left")
        self.reload_btn = tk.Button(bar, text="Reload", command=self.reload,
                                    bg="#2d2d2d", fg=FG, relief="flat",
                                    activebackground="#3a3a3a",
                                    activeforeground=FG, padx=14, pady=4,
                                    state="disabled")
        self.reload_btn.pack(side="left", padx=(6, 0))

        tk.Label(bar, text="filter", bg=BG, fg=MUTED).pack(side="left",
                                                           padx=(18, 4))
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", lambda *_: self._apply_filter())
        e = tk.Entry(bar, textvariable=self.filter_var, bg="#2d2d2d", fg=FG,
                     insertbackground=FG, relief="flat", width=22)
        e.pack(side="left", ipady=3)

        self.path_lbl = tk.Label(self.root, text="no file loaded", bg=BG,
                                 fg=MUTED, anchor="w")
        self.path_lbl.pack(fill="x", padx=12)

    def _build_table(self):
        wrap = tk.Frame(self.root, bg=BG)
        wrap.pack(fill="both", expand=True, padx=12, pady=(8, 4))

        head = tk.Frame(wrap, bg=BG)
        head.pack(fill="x")
        tk.Label(head, text="parameter", bg=BG, fg=MUTED, width=32,
                 anchor="w").pack(side="left")
        tk.Label(head, text="original", bg=BG, fg=MUTED, width=14,
                 anchor="w").pack(side="left")
        tk.Label(head, text="value", bg=BG, fg=MUTED,
                 anchor="w").pack(side="left")

        self.canvas = tk.Canvas(wrap, bg=BG, highlightthickness=0)
        sb = ttk.Scrollbar(wrap, orient="vertical", command=self.canvas.yview)
        self.inner = tk.Frame(self.canvas, bg=BG)
        self.inner.bind("<Configure>", lambda e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=sb.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        # Wheel scrolling has to be bound on the toplevel: the canvas only
        # receives it while the pointer is over empty space, not over a row.
        self.root.bind_all("<MouseWheel>", self._wheel)

    def _build_footer(self):
        foot = tk.Frame(self.root, bg=BG)
        foot.pack(fill="x", padx=12, pady=(4, 12))

        self.add_var = tk.StringVar(value="add attested key…")
        self.add_menu = ttk.Combobox(foot, textvariable=self.add_var,
                                     state="disabled", width=26,
                                     values=[])
        self.add_menu.pack(side="left")
        self.add_menu.bind("<<ComboboxSelected>>", self._add_attested)

        self.save_btn = tk.Button(foot, text="Save as…", command=self.save_as,
                                  bg=ACCENT, fg="#06213f", relief="flat",
                                  activebackground="#7bbcff", padx=16, pady=5,
                                  state="disabled")
        self.save_btn.pack(side="right")
        self.over_btn = tk.Button(foot, text="Overwrite original",
                                  command=self.overwrite, bg="#2d2d2d", fg=FG,
                                  relief="flat", activebackground="#3a3a3a",
                                  activeforeground=FG, padx=12, pady=5,
                                  state="disabled")
        self.over_btn.pack(side="right", padx=(0, 8))
        self.revert_btn = tk.Button(foot, text="Revert", command=self.revert,
                                    bg="#2d2d2d", fg=FG, relief="flat",
                                    activebackground="#3a3a3a",
                                    activeforeground=FG, padx=12, pady=5,
                                    state="disabled")
        self.revert_btn.pack(side="right", padx=(0, 8))

        self.status = tk.Label(self.root, text="", bg="#141414", fg=MUTED,
                               anchor="w", padx=12, pady=5)
        self.status.pack(fill="x", side="bottom")

    def _wire_dnd(self):
        if not HAVE_DND:
            return
        self.root.drop_target_register(DND_FILES)
        self.root.dnd_bind("<<Drop>>", self._on_drop)

    # ---------------- events ----------------

    def _wheel(self, ev):
        self.canvas.yview_scroll(int(-ev.delta / 120), "units")

    def _on_drop(self, ev):
        paths = parse_drop(ev.data)
        if not paths:
            return
        p = Path(paths[0])
        if p.suffix.lower() != ".vcf":
            self._set_status(f"{p.name} is not a .vcf", error=True)
            return
        self.load(p)

    def open_dialog(self):
        p = filedialog.askopenfilename(
            title="Open a VCF",
            filetypes=[("Voice config", "*.vcf"), ("All files", "*.*")])
        if p:
            self.load(Path(p))

    def reload(self):
        if self.path:
            self.load(self.path)

    def revert(self):
        for name, var in self.vars.items():
            var.set(self.orig[name])
        self._refresh_marks()

    # ---------------- load ----------------

    def load(self, path):
        try:
            params = VV.read_params(path)
        except Exception as exc:
            messagebox.showerror("Cannot read VCF",
                                 f"{path}\n\n{type(exc).__name__}: {exc}")
            self._set_status(f"failed to read {path.name}", error=True)
            return
        if not params:
            self._set_status(f"{path.name}: no tts.voiceCfg parameters found "
                             f"-- is this really a VCF?", error=True)
            return

        # ⭐ Prove the codec reproduces THIS file before offering to write it.
        # A VCF whose bytes do not survive decrypt->encrypt unchanged is one
        # this tool must not save over: the difference would be silent.
        try:
            import tempfile
            with tempfile.NamedTemporaryFile(delete=False, suffix=".vcf") as t:
                tmp = t.name
            ok, n_src, n_out = VV.verify_roundtrip(path, tmp)
            Path(tmp).unlink(missing_ok=True)
            self.roundtrip = (ok, n_src, n_out)
        except Exception:
            self.roundtrip = None

        self.path = Path(path)
        self.orig = dict(params)
        self.vars = {k: tk.StringVar(value=v) for k, v in params.items()}
        for var in self.vars.values():
            var.trace_add("write", lambda *_: self._refresh_marks())
        self._render_rows()

        self.path_lbl.config(text=str(self.path), fg=FG)
        for b in (self.reload_btn, self.save_btn, self.over_btn,
                  self.revert_btn):
            b.config(state="normal")
        addable = [k for k in VV.ATTESTED_ADDITIONS if k not in self.orig]
        self.add_menu.config(values=addable,
                             state="readonly" if addable else "disabled")
        self.add_var.set("add attested key…" if addable else "none to add")

        rt = ""
        if self.roundtrip:
            ok, n_src, n_out = self.roundtrip
            rt = ("  ·  codec round-trip OK"
                  if ok else
                  f"  ·  ⛔ ROUND-TRIP MISMATCH ({n_src} in, {n_out} out) "
                  f"-- do not overwrite this file")
        self._set_status(f"{len(params)} parameters{rt}",
                         error=bool(self.roundtrip and not self.roundtrip[0]))

    def _render_rows(self):
        for child in self.inner.winfo_children():
            child.destroy()
        self.rows = {}
        for name in sorted(self.vars):
            row = tk.Frame(self.inner, bg=BG)
            row.pack(fill="x", pady=1)
            nl = tk.Label(row, text=name, bg=BG, fg=FG, width=32, anchor="w",
                          font=("Consolas", 9))
            nl.pack(side="left")
            ol = tk.Label(row, text=self.orig[name], bg=BG, fg=MUTED, width=14,
                          anchor="w", font=("Consolas", 9))
            ol.pack(side="left")
            en = tk.Entry(row, textvariable=self.vars[name], bg="#2d2d2d",
                          fg=FG, insertbackground=FG, relief="flat", width=18,
                          font=("Consolas", 9))
            en.pack(side="left", ipady=2)
            if name in VV.ATTESTED_ADDITIONS and name not in self.orig:
                tk.Label(row, text="  (added)", bg=BG, fg=ACCENT,
                         font=("Consolas", 8)).pack(side="left")
            self.rows[name] = (row, nl, ol, en)
        self._apply_filter()
        self._refresh_marks()

    def _apply_filter(self):
        q = self.filter_var.get().strip().lower()
        for name, (row, *_rest) in self.rows.items():
            if not q or q in name.lower():
                row.pack(fill="x", pady=1)
            else:
                row.pack_forget()

    def _refresh_marks(self):
        n = 0
        for name, (row, nl, ol, en) in self.rows.items():
            changed = self.vars[name].get().strip() != str(
                self.orig.get(name, "")).strip()
            n += changed
            bg = CHANGED if changed else BG
            row.configure(bg=bg)
            nl.configure(bg=bg, fg=ACCENT if changed else FG)
            ol.configure(bg=bg)
        if self.path:
            base = f"{len(self.vars)} parameters"
            extra = f"  ·  {n} changed" if n else "  ·  no changes"
            rt = ""
            if self.roundtrip and not self.roundtrip[0]:
                rt = "  ·  ⛔ codec round-trip MISMATCH"
            self._set_status(base + extra + rt,
                             error=bool(rt))

    # ---------------- add / save ----------------

    def _add_attested(self, _ev=None):
        name = self.add_var.get()
        if name not in VV.ATTESTED_ADDITIONS or name in self.vars:
            return
        why = VV.ATTESTED_ADDITIONS[name]
        if not messagebox.askokcancel(
                f"Add {name}?",
                f"{name} is not in this VCF.\n\nIt is attested because:\n"
                f"  {why}\n\nThe real Speechify server validates against a DTD "
                f"that rejects unknown names, so only attested keys can be "
                f"added. Continue?"):
            self.add_var.set("add attested key…")
            return
        self.vars[name] = tk.StringVar(value="")
        self.vars[name].trace_add("write", lambda *_: self._refresh_marks())
        self.orig[name] = ""          # absent originally -> any value is a change
        self._render_rows()
        addable = [k for k in VV.ATTESTED_ADDITIONS if k not in self.vars]
        self.add_menu.config(values=addable,
                             state="readonly" if addable else "disabled")
        self.add_var.set("add attested key…" if addable else "none to add")

    def _changes(self):
        out = {}
        for name, var in self.vars.items():
            v = var.get().strip()
            if v != str(self.orig.get(name, "")).strip():
                out[name] = v
        return out

    def _write(self, dst):
        changes = self._changes()
        if not changes:
            messagebox.showinfo("Nothing to save", "No values were changed.")
            return False
        added = [k for k in changes if k not in
                 VV.read_params(self.path)]
        blank = [k for k, v in changes.items() if v == ""]
        if blank:
            messagebox.showerror(
                "Empty value",
                "These have no value:\n  " + "\n  ".join(blank)
                + "\n\nA parameter with an empty <value> is not valid.")
            return False
        summary = "\n".join(f"  {k}: {self.orig.get(k, '(absent)') or '(absent)'}"
                            f"  ->  {v}" for k, v in sorted(changes.items()))
        if not messagebox.askokcancel(
                "Save", f"Write {len(changes)} change(s) to\n{dst}\n\n{summary}"):
            return False
        try:
            VV.write_variant(self.path, dst, changes, allow_add=bool(added))
        except SystemExit as exc:
            # write_variant refuses unknown names by raising SystemExit; in a
            # GUI that would close the window silently.
            messagebox.showerror("Refused", str(exc))
            return False
        except Exception as exc:
            messagebox.showerror("Write failed",
                                 f"{type(exc).__name__}: {exc}\n\n"
                                 f"{traceback.format_exc(limit=3)}")
            return False
        return True

    def save_as(self):
        if not self.path:
            return
        dst = filedialog.asksaveasfilename(
            title="Save VCF as",
            initialdir=str(self.path.parent),
            initialfile=f"{self.path.stem}_edited.vcf",
            defaultextension=".vcf",
            filetypes=[("Voice config", "*.vcf")])
        if not dst:
            return
        if self._write(Path(dst)):
            self._set_status(f"wrote {dst}")

    def overwrite(self):
        if not self.path:
            return
        if self.roundtrip and not self.roundtrip[0]:
            messagebox.showerror(
                "Refusing to overwrite",
                "This file does not survive a decrypt/encrypt round trip "
                "unchanged, so writing it back would alter bytes this editor "
                "does not understand.\n\nUse Save as… instead.")
            return
        if not messagebox.askokcancel(
                "Overwrite the original?",
                f"This replaces\n{self.path}\n\nspfy_synth takes the VCF path "
                f"as an argument, so keeping the original untouched and "
                f"pointing at a copy is usually what you want.\n\nOverwrite "
                f"anyway?"):
            return
        if self._write(self.path):
            self._set_status(f"overwrote {self.path.name}")
            self.load(self.path)

    def _set_status(self, text, error=False):
        self.status.config(text=text, fg="#ff8080" if error else MUTED)


def main():
    root = TkinterDnD.Tk() if HAVE_DND else tk.Tk()
    app = Editor(root)
    if len(sys.argv) > 1:
        p = Path(sys.argv[1])
        if p.is_file():
            app.load(p)
        else:
            app._set_status(f"{p} not found", error=True)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
