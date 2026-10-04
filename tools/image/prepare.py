import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def pristine(path):
    original = path + ".orig"
    if not os.path.exists(original):
        shutil.copyfile(path, original)
    with open(original, "rb") as handle:
        return handle.read().decode("latin-1").replace("\r\n", "\n")


def write(path, text):
    with open(path, "wb") as handle:
        handle.write(text.replace("\r\n", "\n").replace("\n", "\r\n").encode("latin-1"))


def replace(text, old, new, path):
    if text.count(old) != 1:
        raise SystemExit(f"{path}: expected one {old!r}")
    return text.replace(old, new)


def patch(path, edits, extra=""):
    text = pristine(path)
    for old, new in edits:
        text = replace(text, old, new, path)
    write(path, text + extra)


def read_apps(apps_folder):
    apps = []
    with open(os.path.join(HERE, "apps.txt")) as handle:
        for line in handle:
            fields = line.split(maxsplit=2)
            if not fields:
                continue
            name = fields[0]
            source = os.path.join(apps_folder, name) if apps_folder else None
            if not source or not os.path.exists(source):
                print(f"prepare: skipping {name} (not in SH3_APPS)")
                continue
            apps.append((name, source, fields[1] if len(fields) > 1 else None, fields[2].strip() if len(fields) > 2 else None))
    return apps


def main():
    tree, debugmgr, netdial = sys.argv[1], sys.argv[2], sys.argv[3]
    apps_folder = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
    files = os.path.join(tree, "PLATFORM", "ODO", "FILES")
    apps = read_apps(apps_folder)

    patch(os.path.join(tree, "PUBLIC", "COMMON", "OAK", "MISC", "SRCGEN1.BAT"), [
        ("\tset ___PUBROOT=%3\n",
         "\tset ___PUBROOT=%_PROJECTROOT%\\cesysgen\n"
         "\tif \"%1\"==\"winceos\" set ___PUBROOT=%_PUBLICROOT%\\common\n"
         "\tif exist %_PUBLICROOT%\\%1\\cesysgen\\makefile set ___PUBROOT=%_PUBLICROOT%\\common\n"),
    ])
    patch(os.path.join(tree, "PUBLIC", "WCESHELL", "OAK", "CTLPNL", "CPLMAIN", "MAKEFILE.INC"), [
        ("\t@del $@ > nul 2>&1", "\t-@del $@ > nul 2>&1"),
    ])
    shx_memory = ("\tIF _TGTCPUTYPE=SHx\n\t\tNK       8C600000  00a00000  RAMIMAGE\n\t\tIF IMGEBOOT\n"
                  "\t\t\t; Ethernet debugging packet buffers\n\t\t\tEDBG\t8C030000  00020000 RESERVED\n"
                  "\t\t\tRAM\t8C050000  005B0000 RAM\n\t\tENDIF\n\t\tIF IMGEBOOT !\n"
                  "\t\t\tRAM      8C030000  005d0000  RAM\n")
    patch(os.path.join(files, "CONFIG.BIB"), [
        (shx_memory, shx_memory.replace("8C600000  00a00000", "8C700000  00900000").replace("005B0000", "006B0000").replace("005d0000", "006d0000")),
    ], "   FSRAMPERCENT=0x40404040\n")

    shutil.copyfile(debugmgr, os.path.join(files, "velo-debugmgr.exe"))
    shutil.copyfile(netdial, os.path.join(files, "netdial.exe"))
    bib = ["", "FILES", "   velo-debugmgr.exe  $(_FLATRELEASEDIR)\\velo-debugmgr.exe  NK  S",
           "   netdial.exe  $(_FLATRELEASEDIR)\\netdial.exe  NK  S"]
    dat = []
    folders = []
    for name, source, folder, title in apps:
        shutil.copyfile(source, os.path.join(files, name))
        bib.append(f"   {name}  $(_FLATRELEASEDIR)\\{name}  NK  S")
        if not folder:
            continue
        link = os.path.splitext(name)[0] + ".lnk"
        target = f"\\Windows\\{name}"
        with open(os.path.join(files, link), "wb") as handle:
            handle.write(f"{len(target)}#{target}".encode("ascii"))
        bib.append(f"   {link}  $(_FLATRELEASEDIR)\\{link}  NK  S")
        if folder not in folders:
            folders.append(folder)
            dat.append(f'Directory("\\Windows\\Programs"):-Directory("{folder}")')
        dat.append(f'Directory("\\Windows\\Programs\\{folder}"):-File("{title}.lnk", "\\Windows\\{link}")')
    patch(os.path.join(files, "PLATFORM.BIB"), [], "\n".join(bib) + "\n")
    with open(os.path.join(HERE, "odo.reg")) as handle:
        patch(os.path.join(files, "PLATFORM.REG"), [], "\n" + handle.read())
    patch(os.path.join(files, "PLATFORM.DAT"), [], "\n".join(dat) + "\n" if dat else "")


main()
