"""Minimal PE32 image loader used by the static recompiler.

Maps the sections of a 32-bit Windows DLL into a flat byte array laid out at
the image's preferred base, and exposes the relocation, import and export
tables that the code discovery and code generation passes need.
"""

import struct

import pefile


class Section:
    def __init__(self, name, va, vsize, characteristics):
        self.name = name
        self.va = va
        self.vsize = vsize
        self.characteristics = characteristics

    @property
    def end(self):
        return self.va + self.vsize

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000000)

    @property
    def writable(self):
        return bool(self.characteristics & 0x80000000)

    def __repr__(self):
        return f"Section({self.name} {self.va:#x}-{self.end:#x})"


class Image:
    def __init__(self, path):
        pe = pefile.PE(path, fast_load=False)
        self.path = path
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytearray(self.size)
        hdr = pe.get_memory_mapped_image()[: pe.OPTIONAL_HEADER.SizeOfHeaders]
        self.mem[: len(hdr)] = hdr
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            vsize = max(s.Misc_VirtualSize, s.SizeOfRawData)
            data = s.get_data()[: max(s.Misc_VirtualSize, 0) or len(s.get_data())]
            off = s.VirtualAddress
            self.mem[off : off + len(data)] = data
            self.sections.append(Section(name, va, vsize, s.Characteristics))

        # HIGHLOW base relocations: the set of VAs holding absolute addresses.
        self.relocs = set()
        if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
            for block in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in block.entries:
                    if e.type == 3:  # IMAGE_REL_BASED_HIGHLOW
                        self.relocs.add(self.base + e.rva)
                    elif e.type != 0:
                        raise ValueError(f"unsupported relocation type {e.type}")

        # Imports: IAT slot VA -> (dll, name)
        self.imports = {}
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else f"ord{imp.ordinal}"
                self.imports[imp.address] = (dll, name)

        # Exports: name -> VA
        self.exports = {}
        for sym in getattr(pe, "DIRECTORY_ENTRY_EXPORT", None).symbols:
            if sym.name:
                self.exports[sym.name.decode()] = self.base + sym.address

        self.text = next(s for s in self.sections if s.executable)

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.end:
                return s
        return None

    def in_image(self, va):
        return self.base <= va < self.base + self.size

    def in_text(self, va):
        return self.text.va <= va < self.text.end

    def read(self, va, n):
        off = va - self.base
        return bytes(self.mem[off : off + n])

    def u8(self, va):
        return self.mem[va - self.base]

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - self.base)[0]
