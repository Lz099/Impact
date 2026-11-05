"""
Symbolise une pile de plantage Unreal sans construire le moteur depuis les sources.

Le moteur installe ses DLL sans fichiers .pdb, donc le rapport de plantage ne donne que des
decalages dans le module. Mais un DLL Unreal exporte presque toutes ses fonctions : en resolvant
chaque decalage vers le symbole exporte qui le precede, on retrouve la fonction. La reponse est une
borne inferieure -- une fonction statique non exportee sortira sous le nom de sa voisine -- et cela
a suffi a trouver la cause du plantage decrit dans Saved/_preuves_reprise/PLANTAGE_INTERMITTENT.md.

Usage :
    python Tools/sym_crash.py Saved/Crashes/UECC-Windows-<...>/CrashContext.runtime-xml
    python Tools/sym_crash.py UnrealEditor-Engine:1a71514 UnrealEditor-Core:458448
"""

import bisect
import glob
import os
import re
import struct
import sys

ENGINE_BINARIES = r"D:\Unreal\UE_5.7\Engine\Binaries\Win64"


def read_exports(path):
    """Rend la liste (rva, nom) des symboles exportes d'un PE, triee par rva."""
    with open(path, "rb") as handle:
        data = handle.read()

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path} n'est pas un PE")

    coff = pe + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    is_pe32_plus = struct.unpack_from("<H", data, optional)[0] == 0x20B
    directories = optional + (112 if is_pe32_plus else 96)
    export_rva = struct.unpack_from("<I", data, directories)[0]
    if export_rva == 0:
        return []

    sections = []
    table = optional + optional_size
    for index in range(section_count):
        base = table + index * 40
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_address))

    def to_offset(rva):
        for virtual_address, size, raw_address in sections:
            if virtual_address <= rva < virtual_address + size:
                return raw_address + (rva - virtual_address)
        raise ValueError(f"rva {rva:#x} hors sections")

    directory = to_offset(export_rva)
    name_count = struct.unpack_from("<I", data, directory + 24)[0]
    functions, names, ordinals = struct.unpack_from("<III", data, directory + 28)
    functions, names, ordinals = to_offset(functions), to_offset(names), to_offset(ordinals)

    exports = []
    for index in range(name_count):
        name_rva = struct.unpack_from("<I", data, names + 4 * index)[0]
        ordinal = struct.unpack_from("<H", data, ordinals + 2 * index)[0]
        rva = struct.unpack_from("<I", data, functions + 4 * ordinal)[0]
        start = to_offset(name_rva)
        exports.append((rva, data[start:data.index(b"\0", start)].decode("latin1")))

    exports.sort()
    return exports


_cache = {}


def resolve(module, offset):
    """Rend 'symbole (+decalage)' pour un decalage dans un module du moteur."""
    if module not in _cache:
        candidates = glob.glob(os.path.join(ENGINE_BINARIES, module + ".dll"))
        candidates += glob.glob(os.path.join(ENGINE_BINARIES, module + ".exe"))
        _cache[module] = read_exports(candidates[0]) if candidates else []

    exports = _cache[module]
    if not exports:
        return f"{module} +{offset:#x}  (module introuvable)"

    index = bisect.bisect_right([rva for rva, _ in exports], offset) - 1
    if index < 0:
        return f"{module} +{offset:#x}  (avant le premier export)"

    rva, name = exports[index]
    return f"{name}  (+{offset - rva:#x})"


def report(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    message = re.search(r"<ErrorMessage>(.*?)</ErrorMessage>", text, re.S)
    if message:
        print(" ".join(message.group(1).split()))
        print()

    stack = re.search(r"<PCallStack>(.*?)</PCallStack>", text, re.S)
    if not stack:
        print("pas de <PCallStack> dans ce rapport")
        return

    for line in stack.group(1).splitlines():
        frame = re.match(r"\s*(\S+)\s+0x\w+\s+\+\s+(\w+)", line)
        if frame:
            print("   ", resolve(frame.group(1), int(frame.group(2), 16)))


def main(argv):
    if not argv:
        print(__doc__)
        return 1

    for argument in argv:
        if ":" in argument and not os.path.exists(argument):
            module, offset = argument.rsplit(":", 1)
            print(f"{argument}  ->  {resolve(module, int(offset, 16))}")
        else:
            report(argument)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
