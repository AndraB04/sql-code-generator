#!/usr/bin/env python3
"""Minimal Markdown-to-PDF converter for the project documentation.

It supports the subset used by docs/SRS.md and docs/SDD.md: headings,
paragraphs, unordered/ordered lists and fenced code blocks.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


LATEX_SPECIALS = {
    "\\": r"\textbackslash{}",
    "&": r"\&",
    "%": r"\%",
    "$": r"\$",
    "#": r"\#",
    "_": r"\_",
    "{": r"\{",
    "}": r"\}",
    "~": r"\textasciitilde{}",
    "^": r"\textasciicircum{}",
}


def latex_escape(text: str) -> str:
    return "".join(LATEX_SPECIALS.get(ch, ch) for ch in text)


def inline_markup(text: str) -> str:
    parts = text.split("`")
    rendered: list[str] = []
    for idx, part in enumerate(parts):
        if idx % 2 == 0:
            rendered.append(latex_escape(part))
        else:
            rendered.append(r"\texttt{" + latex_escape(part) + "}")
    return "".join(rendered)


def close_list(lines: list[str], state: dict[str, str | None]) -> None:
    if state["list"] == "itemize":
        lines.append(r"\end{itemize}")
    elif state["list"] == "enumerate":
        lines.append(r"\end{enumerate}")
    state["list"] = None


def open_list(lines: list[str], state: dict[str, str | None], list_type: str) -> None:
    if state["list"] == list_type:
        return
    close_list(lines, state)
    lines.append(r"\begin{" + list_type + "}")
    state["list"] = list_type


def markdown_to_latex(md_text: str) -> str:
    out: list[str] = []
    state: dict[str, str | None] = {"list": None}
    in_code = False

    for raw in md_text.splitlines():
        line = raw.rstrip()

        if line.startswith("```"):
            close_list(out, state)
            if in_code:
                out.append(r"\end{verbatim}")
                in_code = False
            else:
                out.append(r"\begin{verbatim}")
                in_code = True
            continue

        if in_code:
            out.append(line)
            continue

        if not line.strip():
            close_list(out, state)
            out.append("")
            continue

        heading = re.match(r"^(#{1,4})\s+(.*)$", line)
        if heading:
            close_list(out, state)
            level = len(heading.group(1))
            title = inline_markup(heading.group(2))
            if level == 1:
                out.append(r"\begin{center}")
                out.append(r"{\LARGE\bfseries " + title + r"}")
                out.append(r"\end{center}")
                out.append(r"\vspace{0.5em}")
            elif level == 2:
                out.append(r"\section*{" + title + "}")
            elif level == 3:
                out.append(r"\subsection*{" + title + "}")
            else:
                out.append(r"\subsubsection*{" + title + "}")
            continue

        bullet = re.match(r"^-\s+(.*)$", line)
        if bullet:
            open_list(out, state, "itemize")
            out.append(r"\item " + inline_markup(bullet.group(1)))
            continue

        numbered = re.match(r"^\d+\.\s+(.*)$", line)
        if numbered:
            open_list(out, state, "enumerate")
            out.append(r"\item " + inline_markup(numbered.group(1)))
            continue

        close_list(out, state)
        out.append(inline_markup(line) + r"\par")

    close_list(out, state)
    if in_code:
        out.append(r"\end{verbatim}")
    return "\n".join(out)


def document(body: str) -> str:
    return "\n".join(
        [
            r"\documentclass[11pt,a4paper]{article}",
            r"\usepackage[T1]{fontenc}",
            r"\usepackage[utf8]{inputenc}",
            r"\usepackage[a4paper,margin=2.2cm]{geometry}",
            r"\setlength{\parindent}{0pt}",
            r"\setlength{\parskip}{0.6em}",
            r"\sloppy",
            r"\begin{document}",
            body,
            r"\end{document}",
            "",
        ]
    )


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: md_to_pdf.py input.md output.pdf", file=sys.stderr)
        return 2

    md_path = Path(sys.argv[1])
    pdf_path = Path(sys.argv[2])
    tex_path = pdf_path.with_suffix(".tex")
    aux_dir = pdf_path.parent

    body = markdown_to_latex(md_path.read_text(encoding="utf-8"))
    tex_path.write_text(document(body), encoding="utf-8")

    cmd = [
        "pdflatex",
        "-interaction=nonstopmode",
        "-halt-on-error",
        "-output-directory",
        str(aux_dir),
        str(tex_path),
    ]
    for _ in range(2):
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    tex_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
