#!/usr/bin/env python3
"""Stage-1 triage of the Cossacks executables.

Runs zeus-poseidon-arm64's tools/pe_triage.py (read from origin/main of a zeus clone, never
checked out) on every PE file under GAME_DIR, saves the raw reports, and writes imports.md:
every imported function grouped by DLL, marked with whether the Zeus host already provides it
(a def()/def_in()/p.def() registration in origin/main:src/win32/*.cpp).

- Imports by ordinal are named from the export table of the matching DLL in --sysdir
  (a Wine syswow64 folder), e.g. ws2_32 #23 -> socket.
- Imports from a module shipped with the game (dmcr.exe, iChat.dll, ...) are guest-to-guest
  links: they are checked against that module's export table, not against the host.
- Files below a sub-folder (Missions/..., AI/...) are summarised per folder.

Usage: cossacks_imports.py GAME_DIR ZEUS_CLONE OUT_DIR [--sysdir DIR] [--title TEXT]
Run with `python3 -I`: it only reads game files, it never imports anything from them.
"""
import collections
import hashlib
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

# Role of each top-level file, from the evidence gathered in stage 1 (see assessment.md).
ROLES = {
    "dmcr.exe": "**гра** (рушій, 2002)",
    "mdraw.dll": "обгортка GOG над DirectDraw (2010) — замінити нативно",
    "minmm.dll": "обгортка GOG над winmm: CD-музика з OGG, MCI (2009) — замінити нативно",
    "ichat.dll": "чат і лобі GSC в інтернеті — статичний імпорт гри",
    "intexplorer.dll": "інтернет-меню GSC (LoadLibrary)",
    "gw_server.dll": "сервер мережевої гри GSC",
    "unrar.dll": "розпаковка `.rar` місій (LoadLibrary)",
    "cew.dll": "файл даних, читається через CreateFileA, не завантажується",
    "cossacks.exe": "лаунчер GOG: відео + запуск dmln.exe (UPX)",
    "dmln.exe": "лаунчер (2010) → dmcr.exe",
    "scenarioeditor.exe": "редактор сценаріїв",
    "hview.exe": "переглядач довідки",
    "httpc.exe": "HTTP-клієнт (оновлення)",
    "gameuxinstallhelper.dll": "встановлювач Windows Game Explorer (GOG)",
    "goggame-1207662833.dll": "інтеграція GOG Galaxy",
}

# What the port has to run: the engine and the DLLs it loads for missions and AI.
RUNTIME = {"dmcr.exe", "unrar.dll", "AI/…/*.dll", "History_battl/…/*.dll", "Missions/…/*.dll",
           "UserMissions/…/*.dll"}
# Game-folder modules whose exports the host would provide natively instead of emulating them.
REPLACED = {"mdraw.dll": "ddraw.dll", "minmm.dll": "winmm.dll", "ichat.dll": "ichat.dll (заглушки)"}


def git_show(clone, path):
    return subprocess.run(["git", "-C", clone, "show", f"origin/main:{path}"],
                          capture_output=True, check=True).stdout


def host_functions(clone):
    """name -> set of src/win32 files that register it."""
    files = subprocess.run(["git", "-C", clone, "ls-tree", "--name-only", "origin/main", "src/win32/"],
                           capture_output=True, text=True, check=True).stdout.split()
    defined = collections.defaultdict(set)
    for path in files:
        if not path.endswith(".cpp"):
            continue
        text = git_show(clone, path).decode("utf-8", "replace")
        # same rule as tools/api_imports.py: the last string literal before the arg count
        for m in re.finditer(r'\bdef(?:_in)?\(\s*((?:"[^"]*",\s*)+)\d', text):
            defined[re.findall(r'"([^"]*)"', m.group(1))[-1]].add(os.path.basename(path))
    return defined


def is_pe(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if head[:2] != b"MZ" or len(head) < 0x40:
                return False
            f.seek(struct.unpack_from("<I", head, 0x3C)[0])
            return f.read(4) == b"PE\0\0"
    except (OSError, struct.error):
        return False


def exports(pe):
    """(names, ordinal -> name) from the export directory."""
    rva = pe.dirs[0][0] if pe.dirs else 0
    if not rva:
        return set(), {}
    o = pe.offset(rva)
    base, n_funcs, n_names, _, a_names, a_ords = struct.unpack_from("<6I", pe.d, o + 16)
    names, by_ord = set(), {}
    for i in range(n_names):
        name = pe.cstr(struct.unpack_from("<I", pe.d, pe.offset(a_names) + 4 * i)[0])
        ordinal = base + struct.unpack_from("<H", pe.d, pe.offset(a_ords) + 2 * i)[0]
        names.add(name)
        by_ord[ordinal] = name
    return names, by_ord


def group_of(rel):
    parts = rel.split(os.sep)
    if len(parts) == 1:
        return rel
    return parts[0] + "/…/*" + os.path.splitext(parts[-1])[1].lower()


def main(argv):
    if len(argv) < 4:
        sys.exit(__doc__)
    game, clone, out = (os.path.abspath(os.path.expanduser(a)) for a in argv[1:4])
    sysdir = os.path.expanduser(argv[argv.index("--sysdir") + 1]) if "--sysdir" in argv else None
    title = argv[argv.index("--title") + 1] if "--title" in argv else "Cossacks: Back to War (GOG)"
    analysis = os.path.join(out, "analysis")
    os.makedirs(os.path.join(analysis, "triage"), exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        triage_py = os.path.join(tmp, "pe_triage.py")
        with open(triage_py, "wb") as f:
            f.write(git_show(clone, "tools/pe_triage.py"))
        spec = importlib.util.spec_from_file_location("pe_triage", triage_py)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        PE = mod.PE
        commit = subprocess.run(["git", "-C", clone, "rev-parse", "--short", "origin/main"],
                                capture_output=True, text=True, check=True).stdout.strip()

        pes = []
        for root, dirs, files in os.walk(game):
            dirs.sort()
            for name in sorted(files):
                if is_pe(os.path.join(root, name)):
                    pes.append(os.path.join(root, name))

        info = {}
        for path in pes:
            rel = os.path.relpath(path, game)
            text = subprocess.run([sys.executable, "-I", triage_py, rel, "--imports"],
                                  capture_output=True, text=True, cwd=game)
            with open(os.path.join(analysis, "triage", rel.replace(os.sep, "__") + ".txt"), "w") as f:
                f.write(text.stdout + text.stderr)
            pe = PE(path)
            names, _ = exports(pe)
            info[rel] = dict(pe=pe, imports=pe.imports(), exports=names,
                             sha=hashlib.sha256(pe.d).hexdigest(),
                             dll=bool(struct.unpack_from("<H", pe.d, struct.unpack_from("<I", pe.d, 0x3C)[0] + 22)[0] & 0x2000),
                             reloc=bool(len(pe.dirs) > 5 and pe.dirs[5][0]),
                             tls=bool(len(pe.dirs) > 9 and pe.dirs[9][0]))

    local = {os.path.basename(r).lower(): r for r in info if os.sep not in r}
    ordinal_names = {}

    def resolve(dll, fn):
        if not fn.startswith("#") or not sysdir:
            return fn
        if dll not in ordinal_names:
            path = next((os.path.join(sysdir, n) for n in os.listdir(sysdir) if n.lower() == dll), None)
            ordinal_names[dll] = exports(PE(path))[1] if path else {}
        name = ordinal_names[dll].get(int(fn[1:]))
        return f"{fn} {name}" if name else fn

    defined = host_functions(clone)
    users = collections.defaultdict(lambda: collections.defaultdict(set))  # dll -> fn -> {group}
    for rel, i in info.items():
        for dll, funcs in i["imports"].items():
            for fn in funcs:
                users[dll.lower()][resolve(dll.lower(), fn)].add(group_of(rel))

    def plain(fn):
        return fn.split(" ")[-1]

    groups = collections.defaultdict(list)
    for rel in info:
        groups[group_of(rel)].append(rel)

    with open(os.path.join(analysis, "SHA256SUMS"), "w") as f:
        for rel in sorted(info):
            f.write(f"{info[rel]['sha']}  {rel}\n")

    L = [f"# Імпорти Windows: {title}", "",
         f"Згенеровано `tools/cossacks_imports.py` з `tools/pe_triage.py --imports` "
         f"(upstream zeus-poseidon-arm64, `origin/main` = `{commit}`), "
         f"{time.strftime('%Y-%m-%d')}.", "",
         "- **Хост Zeus ✅** — функцію з такою назвою вже зареєстровано в `src/win32/*.cpp` "
         "(`def`, `def_in`, `p.def`). Це не гарантує тієї самої поведінки, лише що точка входу є.",
         "- **❌** — у хості такої функції немає.",
         "- **🎮** — імпорт з модуля, що лежить у теці гри (`DMCR.EXE`, `ICHAT.DLL`, …): це зв'язок "
         "код гри → код гри, його закриває завантажувач DLL, а не хост. Перевірено за таблицею "
         "експорту цього модуля.",
         "- Номери (`#23`) з системних DLL названо за таблицею експорту відповідної DLL Wine "
         "(CrossOver, `syswow64`).",
         "- Методи COM (DirectDraw, DirectSound, DirectPlay) сюди не потрапляють: їх викликають через vtable.",
         "", "Сирі звіти `pe_triage` лежать у `analysis/triage/`, SHA-256 усіх PE-файлів — у "
         "`analysis/SHA256SUMS`.", "",
         "## Файли PE", "",
         "| Файл | Роль | Тип | Збірка (UTC) | Лінкер | Image base | Reloc | TLS | Експортів | Імпортів |",
         "|---|---|---|---|---|---|---|---|---|---|"]
    for g in sorted(groups, key=lambda g: (os.sep in groups[g][0] or "/" in g, g.lower())):
        rels = groups[g]
        i = info[rels[0]]
        if len(rels) == 1 and g == rels[0]:
            pe = i["pe"]
            L.append(f"| `{g}` | {ROLES.get(g.lower(), '')} | {'DLL' if i['dll'] else 'EXE'} | "
                     f"{time.strftime('%Y-%m-%d', time.gmtime(pe.timestamp))} | {pe.linker[0]}.{pe.linker[1]} | "
                     f"`{pe.image_base:#010x}` | {'так' if i['reloc'] else 'ні'} | {'так' if i['tls'] else 'ні'} | "
                     f"{len(i['exports'])} | {sum(len(v) for v in i['imports'].values())} |")
        else:
            bases = sorted({info[r]["pe"].image_base for r in rels})
            years = sorted({time.gmtime(info[r]["pe"].timestamp).tm_year for r in rels})
            exp = sorted({e for r in rels for e in info[r]["exports"]})
            linkers = "/".join(sorted({"%d.%d" % info[r]["pe"].linker for r in rels}))
            L.append(f"| `{g}` ×{len(rels)} | DLL сценаріїв/AI | DLL | {years[0]}–{years[-1]} | {linkers} | "
                     + ", ".join(f"`{b:#010x}`" for b in bases)
                     + f" | {'так' if all(info[r]['reloc'] for r in rels) else 'не всі'} | "
                     f"{'так' if any(info[r]['tls'] for r in rels) else 'ні'} | "
                     + ", ".join(f"`{e}`" for e in exp[:6]) + (" …" if len(exp) > 6 else "")
                     + f" | {min(sum(len(v) for v in info[r]['imports'].values()) for r in rels)}–"
                     f"{max(sum(len(v) for v in info[r]['imports'].values()) for r in rels)} |")
    L.append("")

    sys_dlls = [d for d in users if d not in local]
    game_dlls = [d for d in users if d in local]

    def needed(fns):
        return {fn for fn, who in fns.items() if who & RUNTIME}

    # --- what the game itself needs from the host -------------------------------------------
    rt = {}  # host-side dll label -> set of functions
    for dll in sys_dlls:
        if needed(users[dll]):
            rt[dll] = needed(users[dll])
    for dll, label in REPLACED.items():
        if dll in users and needed(users[dll]):
            rt[f"{dll} → {label}"] = needed(users[dll])
    rt_total = sum(len(v) for v in rt.values())
    rt_have = sum(1 for v in rt.values() for fn in v if plain(fn) in defined)
    L += ["## Підсумок: що потрібно саме грі", "",
          "Рахуються імпорти `dmcr.exe`, DLL місій і AI (`Missions`, `History_battl`, `UserMissions`, "
          "`AI`) та `unrar.dll`. Обгортки GOG `mdraw.dll` і `minmm.dll` та лобі `iChat.dll` хост "
          "замінює власними функціями, тому враховано функції, які гра бере з них, а не їхні власні "
          "імпорти. Лаунчери, редактор, довідка, інсталятор і мережеві модулі, які гра вантажить "
          "лише в онлайн-меню (`intExplorer.dll`, `gw_server.dll`), сюди не входять.", "",
          f"Потрібно **{rt_total}** функцій, у хості Zeus уже є **{rt_have}**, "
          f"треба написати **{rt_total - rt_have}**.", "",
          "| DLL | Потрібно | Є в хості Zeus | Бракує | Яких бракує |", "|---|---|---|---|---|"]
    for label in sorted(rt, key=lambda d: (-len(rt[d]), d)):
        fns = rt[label]
        miss = sorted((fn for fn in fns if plain(fn) not in defined), key=lambda f: plain(f).lower())
        L.append(f"| `{label}` | {len(fns)} | {len(fns) - len(miss)} | {len(miss)} | "
                 + ", ".join(f"`{fn}`" for fn in miss) + " |")
    L.append("")

    total = sum(len(users[d]) for d in sys_dlls)
    have = sum(1 for d in sys_dlls for fn in users[d] if plain(fn) in defined)
    L += ["## Підсумок: системні DLL, усі файли", "",
          f"Унікальних пар DLL+функція з системних бібліотек: **{total}**, з них у хості Zeus "
          f"уже є **{have}**, бракує **{total - have}**. Сюди входять і інструменти (редактор, "
          "довідка, лаунчери, інсталятор GOG), і обгортки GOG.", "",
          "| DLL | Функцій | Є в хості Zeus | Бракує | Хто імпортує |", "|---|---|---|---|---|"]
    for dll in sorted(sys_dlls, key=lambda d: (-len(users[d]), d)):
        fns = users[dll]
        ok = sum(1 for fn in fns if plain(fn) in defined)
        who = sorted({g for v in fns.values() for g in v}, key=str.lower)
        L.append(f"| `{dll}` | {len(fns)} | {ok} | {len(fns) - ok} | " + ", ".join(f"`{w}`" for w in who) + " |")
    L += ["", "## Підсумок: модулі самої гри (🎮)", "",
          "| Модуль | Функцій | Знайдено в його експорті | Хто імпортує |", "|---|---|---|---|"]
    for dll in sorted(game_dlls, key=lambda d: (-len(users[d]), d)):
        fns = users[dll]
        ok = sum(1 for fn in fns if fn in info[local[dll]]["exports"])
        who = sorted({g for v in fns.values() for g in v}, key=str.lower)
        L.append(f"| `{dll}` | {len(fns)} | {ok} | " + ", ".join(f"`{w}`" for w in who) + " |")
    L.append("")

    for dll in sorted(sys_dlls, key=lambda d: (-len(users[d]), d)) + sorted(game_dlls, key=lambda d: (-len(users[d]), d)):
        fns = users[dll]
        L += [f"## `{dll}` ({len(fns)})", "", "| Функція | Хост Zeus | Грі | Хто імпортує |", "|---|---|---|---|"]
        for fn in sorted(fns, key=lambda f: (plain(f).lower(), f)):
            host = ("✅ " + ", ".join(sorted(defined[plain(fn)]))) if plain(fn) in defined else "❌"
            if dll in local:
                mark = "🎮 є в експорті" if fn in info[local[dll]]["exports"] else "🎮 ⚠️ немає в експорті"
                if dll in REPLACED:
                    mark += f"; у хості: {host}"
            else:
                mark = host
            who = sorted(fns[fn], key=str.lower)
            L.append(f"| `{fn}` | {mark} | {'так' if fns[fn] & RUNTIME else ''} | "
                     + ", ".join(f"`{w}`" for w in who) + " |")
        L.append("")

    with open(os.path.join(out, "imports.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"{len(pes)} PE files; system imports {total}, {have} already in the Zeus host; "
          f"game-module imports {sum(len(users[d]) for d in game_dlls)}")


if __name__ == "__main__":
    main(sys.argv)
