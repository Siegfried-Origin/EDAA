"""Format the add-on C++ sources: clang-format (.clang-format at the repo
root), then the blank-line rules clang-format cannot express:

  * one empty line before and after an if / for / while / switch / do block
    (not at the start or end of a scope, not between "}" and "else");
  * two empty lines between function definitions.

Usage: python tools/format.py [files...]
(default: addon/src/*.cpp, *.hpp and addon/shaders/*.hlsl, formatted as C++)
clang-format is looked up in PATH, then in the Visual Studio LLVM folder.
"""

import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTROL = re.compile(r"^(if|for|while|switch|do)\b")
TYPE_KEYWORDS = re.compile(r"^(struct|class|enum|union|namespace|cbuffer)\b")
OPENING_DIRECTIVE = re.compile(r"^#\s*(if|ifdef|ifndef|else|elif)\b")
CLOSING_DIRECTIVE = re.compile(r"^#\s*(endif|else|elif)\b")


def find_clang_format():
    exe = shutil.which("clang-format")
    if exe:
        return exe
    for path in glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\Llvm\x64\bin\clang-format.exe"):
        return path
    sys.exit("clang-format not found (install the C++ Clang tools for Visual Studio)")


def indent(line):
    return len(line) - len(line.lstrip(" "))


def next_nonblank(lines, i):
    while i < len(lines) and not lines[i].strip():
        i += 1
    return i


def prev_nonblank(lines, i):
    while i >= 0 and not lines[i].strip():
        i -= 1
    return i


def closing_brace(lines, open_line, level):
    """Index of the "}" closing the block opened at open_line (indent level)."""
    for k in range(open_line + 1, len(lines)):
        if lines[k].strip().startswith("}") and indent(lines[k]) == level:
            return k
    return None


def block_end(lines, start):
    """Last line of a control block starting at 'start', else-chain included."""
    level = indent(lines[start])
    i = start
    while True:
        j = i
        while j < len(lines) and not lines[j].rstrip().endswith("{"):
            j += 1
        if j == len(lines):
            return None
        k = closing_brace(lines, j, level)
        if k is None:
            return None
        n = next_nonblank(lines, k + 1)
        if n < len(lines) and lines[n].strip().startswith("else") and indent(lines[n]) == level:
            i = n
            continue
        # "} else {" on one line is not produced by the style, but handle it.
        if re.match(r"^}\s*else\b", lines[k].strip()):
            i = k
            continue
        return k


def is_separator_free(line):
    """Lines after which no empty line is added: scope or label starts."""
    s = line.strip()
    return s.endswith("{") or s.endswith(":") or bool(OPENING_DIRECTIVE.match(s))


def blank_rules(lines):
    """Returns ({line: minimum blanks before}, {line: exact blanks before})."""
    minimum, exact = {}, {}

    # Control blocks.
    for i, line in enumerate(lines):
        s = line.strip()
        if not CONTROL.match(s):
            continue
        p = prev_nonblank(lines, i - 1)
        if p >= 0 and lines[p].strip().startswith("}") and s.startswith("while") and lines[p].strip() == "}":
            continue  # do { } while: the while belongs to the block above
        end = block_end(lines, i)
        if end is None:
            continue
        # Before: above the comment lines that introduce the block.
        anchor = i
        while anchor - 1 >= 0 and lines[anchor - 1].strip().startswith("//") and indent(lines[anchor - 1]) == indent(line):
            anchor -= 1
        p = prev_nonblank(lines, anchor - 1)
        if p >= 0 and p == anchor - 1 and not is_separator_free(lines[p]):
            minimum[anchor] = 1
        # After.
        n = next_nonblank(lines, end + 1)
        if n < len(lines):
            ns = lines[n].strip()
            if not (ns.startswith("}") or ns.startswith("case ") or ns.startswith("default:") or ns == "break;"
                    or CLOSING_DIRECTIVE.match(ns) or ns.startswith("else")):
                minimum[n] = max(minimum.get(n, 0), 1)

    # Function definitions: "{" alone on its line after a signature.
    for i, line in enumerate(lines):
        if line.strip() != "{":
            continue
        level = indent(line)
        start = i - 1
        while start > 0 and indent(lines[start]) > level and lines[start].strip():
            start -= 1
        sig = lines[start].strip()
        if not sig or TYPE_KEYWORDS.match(sig) or sig.endswith(";") or sig.endswith("}") or sig.startswith("#"):
            continue
        if indent(lines[start]) != level or "extern \"C\"" in sig:
            continue
        end = closing_brace(lines, i, level)
        if end is None:
            continue
        while start - 1 >= 0 and lines[start - 1].strip().startswith("//") and indent(lines[start - 1]) == level:
            start -= 1
        # A function opening a "#if" section: the separation goes above the "#if".
        if start - 1 >= 0 and OPENING_DIRECTIVE.match(lines[start - 1].strip()):
            start -= 1
        p = prev_nonblank(lines, start - 1)
        if p >= 0 and not is_separator_free(lines[p]):
            exact[start] = 2
        n = next_nonblank(lines, end + 1)
        if n < len(lines):
            ns = lines[n].strip()
            if ns.startswith("}") or CLOSING_DIRECTIVE.match(ns):
                exact[n] = 0
            else:
                exact[n] = 2
    return minimum, exact


def apply_blank_rules(text):
    lines = text.split("\n")
    trailing_newline = lines and lines[-1] == ""
    if trailing_newline:
        lines.pop()
    minimum, exact = blank_rules(lines)
    out, blanks = [], 0
    for i, line in enumerate(lines):
        if not line.strip():
            blanks += 1
            continue
        count = exact.get(i, max(blanks, minimum.get(i, 0))) if out else 0
        out.extend([""] * count)
        out.append(line.rstrip())
        blanks = 0
    return "\n".join(out) + ("\n" if trailing_newline else "")


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "addon", "src", "*.cpp")) +
                                   glob.glob(os.path.join(ROOT, "addon", "src", "*.hpp")) +
                                   glob.glob(os.path.join(ROOT, "addon", "shaders", "*.hlsl")))
    subprocess.run([find_clang_format(), "-i", "--style=file"] + files, check=True, cwd=ROOT)
    for path in files:
        with open(path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        text = text.replace("\r\n", "\n")
        result = apply_blank_rules(text)
        if crlf:
            result = result.replace("\n", "\r\n")
        if result != text:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(result)
        print("formatted", os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
