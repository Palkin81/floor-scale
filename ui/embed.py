#!/usr/bin/env python3
"""Вшивает ui/page.html в константу PAGE файла lib/ui_server.h.

    python3 ui/embed.py           # обновить lib/ui_server.h
    python3 ui/embed.py --check   # только проверить, что файлы синхронны (код возврата 1, если нет)

Источник правды для страницы — ui/page.html. Править PAGE в lib/ui_server.h
вручную не нужно: следующий запуск скрипта перезапишет её.
Из страницы убираются отступы и пустые строки, чтобы ESP отдавал меньше байт.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "ui" / "page.html"
HDR = ROOT / "lib" / "ui_server.h"
PATTERN = re.compile(r'(static const char PAGE\[\] = R"HTML\()(.*?)(\)HTML";)', re.S)


def build_page() -> str:
    lines = [line.strip() for line in SRC.read_text(encoding="utf-8").splitlines()]
    page = "\n".join(line for line in lines if line) + "\n"
    if ')HTML"' in page:
        sys.exit('Ошибка: в page.html встретилась последовательность )HTML" — она закроет raw-строку C++.')
    return page


def main() -> int:
    text = HDR.read_text(encoding="utf-8")
    if not PATTERN.search(text):
        sys.exit("Ошибка: в lib/ui_server.h не найдена константа PAGE.")
    page = build_page()
    updated = PATTERN.sub(lambda m: m.group(1) + "\n" + page + m.group(3), text, count=1)
    if "--check" in sys.argv:
        if updated != text:
            print("lib/ui_server.h не синхронизирован с ui/page.html: запустите python3 ui/embed.py")
            return 1
        print("OK: lib/ui_server.h совпадает с ui/page.html (%d байт страницы)" % len(page.encode("utf-8")))
        return 0
    HDR.write_text(updated, encoding="utf-8")
    print("Вшито: %d байт страницы в lib/ui_server.h" % len(page.encode("utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
