#!/usr/bin/env python3
"""
Scrape game names from https://3ds.jdbye.com and write a JSON file
compatible with gen_title_db.py.

Usage:
    python tools/scrape_jdbye.py [-o tools/jdbye.json]

Then regenerate the title DB:
    python tools/gen_title_db.py tools/system_apps.json tools/jdbye.json \
        tools/list_US.json tools/list_GB.json tools/list_JP.json tools/list_KR.json
"""

import argparse
import json
import time
import urllib.request
from html.parser import HTMLParser

BASE_URL = "https://3ds.jdbye.com/?details={region}"
REGIONS  = ["USA", "EUR", "JAP", "KOR"]
DELAY    = 1.0   # seconds between requests — be polite


class TableParser(HTMLParser):
    """Collect <td> text from every <tr> in the page."""

    def __init__(self):
        super().__init__()
        self._in_td  = False
        self._cell   = []
        self._row    = []
        self.rows    = []

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self._row = []
        elif tag == "td":
            self._in_td = True
            self._cell  = []

    def handle_endtag(self, tag):
        if tag == "td":
            self._in_td = False
            self._row.append("".join(self._cell).strip())
        elif tag == "tr":
            if self._row:
                self.rows.append(self._row)

    def handle_data(self, data):
        if self._in_td:
            self._cell.append(data)

    def handle_entityref(self, name):
        if self._in_td:
            entities = {"amp": "&", "lt": "<", "gt": ">", "quot": '"',
                        "apos": "'", "nbsp": " "}
            self._cell.append(entities.get(name, ""))

    def handle_charref(self, name):
        if self._in_td:
            try:
                code = int(name[1:], 16) if name.startswith("x") else int(name)
                self._cell.append(chr(code))
            except ValueError:
                pass


def fetch_region(region):
    url = BASE_URL.format(region=region)
    print(f"  Fetching {url} ...", end=" ", flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "3ds-activity-sync/titledb-scraper"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        html = resp.read().decode("utf-8", errors="replace")
    parser = TableParser()
    parser.feed(html)

    entries = {}
    for row in parser.rows:
        # Expected columns: Region | Name | Product Code | Title ID
        if len(row) < 4:
            continue
        name   = row[1].strip()
        tid_str = row[3].strip().upper()
        if not name or len(tid_str) != 16:
            continue
        try:
            tid = int(tid_str, 16)
        except ValueError:
            continue
        if tid not in entries:
            entries[tid] = name

    print(f"{len(entries)} entries")
    return entries


def main():
    parser = argparse.ArgumentParser(
        description="Scrape 3DS title names from 3ds.jdbye.com")
    parser.add_argument("-o", "--output", default="tools/jdbye.json",
                        metavar="output.json")
    args = parser.parse_args()

    merged = {}
    for i, region in enumerate(REGIONS):
        entries = fetch_region(region)
        added = sum(1 for tid in entries if tid not in merged)
        merged.update({tid: name for tid, name in entries.items()
                       if tid not in merged})
        print(f"    {added} new unique entries from {region}")
        if i < len(REGIONS) - 1:
            time.sleep(DELAY)

    out = [{"TitleID": f"{tid:016X}", "Name": name}
           for tid, name in sorted(merged.items())]

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)

    print(f"\nWrote {len(out)} entries to {args.output}")


if __name__ == "__main__":
    main()
