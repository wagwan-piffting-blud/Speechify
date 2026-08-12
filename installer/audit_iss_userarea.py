"""Fail if spfy_setup.iss can touch a user's working tree destructively.

WHY THIS EXISTS. {userdocs}\\Speechify is the installer's per-user data
directory AND, on at least one machine, the repository checkout itself. An
uninstall removed 729 tracked source files from that tree, because Inno
deletes everything it installed and an earlier revision of the script
installed the FE tables there. The build then linked an empty asset blob.

The rule this enforces:

  {app}       ours. Install, overwrite and delete freely.
  {userdocs}  a guest. Install only what is missing (onlyifdoesntexist) and
              never remove anything (uninsneveruninstall). No [UninstallDelete]
              and no [InstallDelete] may name a path under it.

Run from anywhere:  python installer/audit_iss_userarea.py
"""
import re
import sys
from pathlib import Path

ISS = Path(sys.argv[1]) if len(sys.argv) > 1 \
    else Path(__file__).resolve().parent / "spfy_setup.iss"
USER_AREAS = ("{userdocs}", "{userappdata}", "{userprofile}", "{userdesktop}",
              "{usersavedgames}", "{userdocs64}")
REQUIRED = ("onlyifdoesntexist", "uninsneveruninstall")

def strip_comment(line):
    """Inno comments start with ';' at the first non-space character."""
    s = line.lstrip()
    return "" if s.startswith(";") else line

def main():
    text = ISS.read_text(encoding="utf-8", errors="replace")
    section, problems, checked = "", [], 0

    for n, raw in enumerate(text.splitlines(), 1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        m = re.match(r"^\s*\[(\w+)\]\s*$", line)
        if m:
            section = m.group(1).lower()
            continue

        low = line.lower()
        hits = [a for a in USER_AREAS if a in low]
        if not hits:
            continue

        if section == "files":
            dest = re.search(r"DestDir:\s*\"([^\"]+)\"", line, re.I)
            if not dest or not any(a in dest.group(1).lower()
                                   for a in USER_AREAS):
                continue                      # user area only in Source: fine
            checked += 1
            flags = re.search(r"Flags:\s*([^;]+)", line, re.I)
            have = (flags.group(1).lower().split() if flags else [])
            missing = [f for f in REQUIRED if f not in have]
            if missing:
                problems.append(
                    f"  line {n}: [Files] into {dest.group(1)} is missing "
                    f"{' and '.join(missing)}\n      {line.strip()[:96]}")
        elif section in ("uninstalldelete", "installdelete"):
            checked += 1
            problems.append(
                f"  line {n}: [{section}] names a user-area path — this is "
                f"how a working tree gets deleted\n      {line.strip()[:96]}")
        elif section == "dirs":
            flags = re.search(r"Flags:\s*([^;]+)", line, re.I)
            have = (flags.group(1).lower().split() if flags else [])
            checked += 1
            if "uninsneveruninstall" not in have:
                problems.append(
                    f"  line {n}: [Dirs] under a user area without "
                    f"uninsneveruninstall\n      {line.strip()[:96]}")

    print(f"audited {ISS.name}: {checked} user-area entr(y/ies)")
    if problems:
        print("\nUNSAFE:")
        print("\n".join(problems))
        print("\nFAIL — the installer can damage a user's working tree.")
        return 1
    print("PASS — every user-area entry is install-only and never removed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
