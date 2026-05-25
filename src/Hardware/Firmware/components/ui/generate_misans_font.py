#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path
import re


DEFAULT_MISANS_URL = (
    "https://raw.githubusercontent.com/boyan01/mi_sans_font/main/lib/fonts/"
    "MiSans-Bold.ttf"
)
DEFAULT_OUTPUT_NAME = "lv_font_misans_bold_16_gb2312"
COMMON_SYMBOLS = (
    "，。！？；：、（）《》〈〉【】「」『』〔〕［］｛｝"
    "“”‘’—…·•￥％＋－＝／＼｜＠＃＆＊℃"
    "→←↑↓"
)


def unique_chars(text: str) -> str:
    return "".join(dict.fromkeys(text))


def build_ascii_chars() -> str:
    return "".join(chr(codepoint) for codepoint in range(0x20, 0x7F))


def build_gb2312_han_chars() -> str:
    chars: list[str] = []
    seen: set[str] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                char = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(char) != 1:
                continue
            if not ("\u4e00" <= char <= "\u9fff"):
                continue
            if char in seen:
                continue
            seen.add(char)
            chars.append(char)
    return "".join(chars)


def load_lv_symbol_codepoints(symbol_file: Path) -> list[int]:
    pattern = re.compile(r"#define\s+LV_SYMBOL_[A-Z0-9_]+\s+\".*?\"\s+/\*\d+,\s+0x([0-9A-Fa-f]+)\*/")
    codepoints: list[int] = []
    seen: set[int] = set()
    for line in symbol_file.read_text(encoding="utf-8").splitlines():
        match = pattern.search(line)
        if not match:
            continue
        codepoint = int(match.group(1), 16)
        if codepoint in seen:
            continue
        seen.add(codepoint)
        codepoints.append(codepoint)
    return codepoints


def download_font(url: str) -> Path:
    temp_dir = Path(tempfile.mkdtemp(prefix="misans-font-"))
    font_path = temp_dir / Path(url).name
    urllib.request.urlretrieve(url, font_path)
    return font_path


def resolve_command(*candidates: str) -> str:
    for candidate in candidates:
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise FileNotFoundError(f"Unable to find any of: {', '.join(candidates)}")


def resolve_node_command() -> str:
    if os.name == "nt":
        return resolve_command("node.exe", "node")
    return resolve_command("node")


def resolve_npm_command() -> str:
    if os.name == "nt":
        return resolve_command("npm.cmd", "npm")
    return resolve_command("npm")


def ensure_lv_font_conv_cli() -> Path:
    cache_root = Path(tempfile.gettempdir()) / "lv_font_conv_cache"
    package_root = cache_root / "node_modules" / "lv_font_conv"
    cli_path = package_root / "lv_font_conv.js"
    if cli_path.exists():
        return cli_path

    cache_root.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [resolve_npm_command(), "install", "--no-save", "lv_font_conv"],
        cwd=cache_root,
        check=True,
    )
    if not cli_path.exists():
        raise FileNotFoundError(f"lv_font_conv.js not found at {cli_path}")
    return cli_path


def normalize_generated_include(output_path: Path) -> None:
    content = output_path.read_text(encoding="utf-8")
    include_blocks = (
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif',
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n    #include "lvgl.h"\n#else\n    #include "../../lvgl.h"\n#endif',
    )

    updated = content
    for include_block in include_blocks:
        updated = updated.replace(include_block, '#include "lvgl.h"', 1)

    if updated == content:
        updated = updated.replace('#include "lvgl/lvgl.h"', '#include "lvgl.h"', 1)

    if '#include "lvgl/lvgl.h"' in updated:
        raise RuntimeError(f"Failed to normalize LVGL include in {output_path}")

    if updated != content:
        output_path.write_text(updated, encoding="utf-8")


def main() -> None:
    this_dir = Path(__file__).resolve().parent
    repo_root = this_dir.parents[1]
    lv_symbol_file = repo_root / "components" / "display" / "lv_v9.3" / "src" / "font" / "lv_symbol_def.h"
    fontawesome_file = (
        repo_root
        / "components"
        / "display"
        / "lv_v9.3"
        / "scripts"
        / "built_in_font"
        / "FontAwesome5-Solid+Brands+Regular.woff"
    )

    parser = argparse.ArgumentParser(description="Generate the MiSans Bold LVGL font.")
    parser.add_argument("--font-path", type=Path, help="Path to a local MiSans-Bold.ttf")
    parser.add_argument("--font-url", default=DEFAULT_MISANS_URL, help="MiSans-Bold.ttf source URL")
    parser.add_argument(
        "--output",
        type=Path,
        default=this_dir / f"{DEFAULT_OUTPUT_NAME}.c",
        help="Output C file path",
    )
    parser.add_argument(
        "--charset-output",
        type=Path,
        default=this_dir / f"{DEFAULT_OUTPUT_NAME}.charset.txt",
        help="Output charset snapshot path",
    )
    parser.add_argument(
        "--lv-font-name",
        default=DEFAULT_OUTPUT_NAME,
        help="Generated LVGL font symbol name",
    )
    args = parser.parse_args()

    misans_font = args.font_path.resolve() if args.font_path else download_font(args.font_url)
    output_path = args.output.resolve()
    charset_path = args.charset_output.resolve()

    ascii_chars = build_ascii_chars()
    han_chars = build_gb2312_han_chars()
    charset = unique_chars(ascii_chars + COMMON_SYMBOLS + han_chars)
    lv_symbol_codepoints = [
        codepoint for codepoint in load_lv_symbol_codepoints(lv_symbol_file) if codepoint >= 0xF000
    ]

    charset_path.write_text(charset, encoding="utf-8")

    cmd = [
        resolve_node_command(),
        str(ensure_lv_font_conv_cli()),
        "--size",
        "16",
        "--bpp",
        "4",
        "--format",
        "lvgl",
        "--no-compress",
        "--no-prefilter",
        "--force-fast-kern-format",
        "--font",
        str(misans_font),
        "--symbols",
        charset,
        "--font",
        str(fontawesome_file),
        "--range",
        ",".join(hex(codepoint) for codepoint in lv_symbol_codepoints),
        "--lv-font-name",
        args.lv_font_name,
        "--output",
        str(output_path),
    ]
    subprocess.run(cmd, check=True)
    normalize_generated_include(output_path)

    print(f"Generated {output_path}")
    print(f"Charset snapshot: {charset_path}")
    print(f"Han chars: {len(han_chars)}")
    print(f"Total chars with ASCII and punctuation: {len(charset)}")
    print(f"LVGL symbol glyphs: {len(lv_symbol_codepoints)}")


if __name__ == "__main__":
    main()