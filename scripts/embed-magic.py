#!/usr/bin/env python3
"""Regenerate src/utils/ngmagic_data.cpp from src/resources/magic.dat."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "resources" / "magic.dat"
OUT = ROOT / "src" / "utils" / "ngmagic_data.cpp"


def main() -> None:
	data = SRC.read_bytes()
	lines = [
		"// Auto-generated from src/resources/magic.dat (CodeWalker-style obfuscated NG tables).",
		"// Do not edit. Regenerate: python3 scripts/embed-magic.py",
		'#include "ngmagic.h"',
		"#include <cstddef>",
		"#include <cstdint>",
		"",
		"namespace ngmagic",
		"{",
		f"\tconst unsigned char kData[{len(data)}] = {{",
	]
	for i in range(0, len(data), 16):
		chunk = data[i : i + 16]
		lines.append("\t\t" + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
	lines.append("\t};")
	lines.append(f"\tconst size_t kDataSize = {len(data)};")
	lines.append("}")
	lines.append("")
	OUT.write_text("\n".join(lines))
	print(f"wrote {OUT} ({OUT.stat().st_size} bytes from {len(data)}-byte magic.dat)")


if __name__ == "__main__":
	main()
