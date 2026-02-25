#!/usr/bin/env python3
"""
Generate source/title_db_data.c from one or more 3DS title database JSON files.

Usage:
    python tools/gen_title_db.py <input1.json> [input2.json ...] [-o output.c]

    output.c defaults to source/title_db_data.c if -o is not specified.
    Files listed first take priority on duplicate title IDs.

Recommended sources (https://github.com/hax0kartik/3dsdb):
    curl -L -o list_US.json "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_US.json"
    curl -L -o list_GB.json "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_GB.json"
    curl -L -o list_JP.json "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_JP.json"
    curl -L -o list_KR.json "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_KR.json"

    python tools/gen_title_db.py tools/system_apps.json \
        tools/list_US.json tools/list_GB.json tools/list_JP.json tools/list_KR.json \
        tools/jdbye.json

jdbye.json is a fallback source scraped from 3ds.jdbye.com. It should be
listed last so the primary list_*.json files take priority on duplicates.

Supported input formats:

  Array of objects:
    [{"TitleID": "0004000000030800", "Name": "Super Mario 3D Land"}, ...]

  Dict of objects (title_id as key):
    {"0004000000030800": {"name": "Super Mario 3D Land", ...}, ...}

  Newline-delimited JSON (one object per line) is also handled as a fallback.

Names longer than 63 bytes (UTF-8) are truncated at a safe boundary.
"""

import argparse
import json
import os
import sys

NAME_MAX = 63  # bytes, excluding null terminator


def escape_c(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def truncate_utf8(s):
    b = s.encode('utf-8')
    if len(b) <= NAME_MAX:
        return s
    b = b[:NAME_MAX]
    # Drop any trailing incomplete UTF-8 sequence
    while b and (b[-1] & 0x80):
        last = b[-1]
        b = b[:-1]
        if (last & 0xC0) == 0xC0:  # start byte — we just removed a complete sequence
            break
    return b.decode('utf-8', errors='ignore')


def parse_array(data):
    entries = {}
    for item in data:
        tid_str = (item.get('TitleID') or item.get('titleID') or
                   item.get('title_id') or item.get('tid') or '').strip()
        name = (item.get('Name') or item.get('name') or '').strip()
        if not tid_str or not name:
            continue
        try:
            tid = int(tid_str, 16)
        except ValueError:
            continue
        if tid not in entries:
            entries[tid] = name
    return entries


def parse_dict(data):
    entries = {}
    for tid_str, val in data.items():
        name = ''
        if isinstance(val, dict):
            name = (val.get('Name') or val.get('name') or '').strip()
        elif isinstance(val, str):
            name = val.strip()
        if not name:
            continue
        try:
            tid = int(tid_str.strip(), 16)
        except ValueError:
            continue
        if tid not in entries:
            entries[tid] = name
    return entries


def parse_file(path):
    with open(path, 'r', encoding='utf-8') as f:
        raw = f.read()

    try:
        data = json.loads(raw)
        if isinstance(data, list):
            return parse_array(data)
        elif isinstance(data, dict):
            return parse_dict(data)
        else:
            print(f'  Unrecognised JSON structure in {path}', file=sys.stderr)
            return {}
    except json.JSONDecodeError:
        # Fall back to newline-delimited JSON
        entries = {}
        for lineno, line in enumerate(raw.splitlines(), 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                print(f'  Skipping line {lineno} in {path}: {e}', file=sys.stderr)
                continue
            if isinstance(obj, dict):
                for tid, name in parse_array([obj]).items():
                    if tid not in entries:
                        entries[tid] = name
        return entries


def main():
    parser = argparse.ArgumentParser(
        description='Generate title_db_data.c from 3DS title database JSON files.')
    parser.add_argument('inputs', nargs='+', metavar='input.json',
                        help='One or more JSON database files (first file wins on duplicates)')
    parser.add_argument('-o', '--output', default='source/title_db_data.c',
                        metavar='output.c',
                        help='Output C source file (default: source/title_db_data.c)')
    args = parser.parse_args()

    merged = {}
    for path in args.inputs:
        print(f'Reading {path}...')
        file_entries = parse_file(path)
        added = 0
        for tid, name in file_entries.items():
            if tid not in merged:
                merged[tid] = name
                added += 1
        print(f'  {len(file_entries)} entries found, {added} new')

    sorted_entries = sorted(merged.items())
    source_names = ', '.join(os.path.basename(p) for p in args.inputs)

    with open(args.output, 'w', encoding='utf-8') as f:
        f.write('/* Auto-generated by tools/gen_title_db.py — do not edit manually.\n')
        f.write(f' * Sources: {source_names}\n')
        f.write(' * Re-run the script to update.\n')
        f.write(' */\n')
        f.write('#include "title_db.h"\n\n')
        f.write('const TitleDbEntry title_db[] = {\n')
        if sorted_entries:
            for tid, name in sorted_entries:
                safe = escape_c(truncate_utf8(name))
                f.write(f'    {{ 0x{tid:016X}ULL, "{safe}" }},\n')
        else:
            f.write('    { 0x0000000000000000ULL, "" }, /* stub */\n')
        f.write('};\n\n')
        f.write('const int title_db_count =\n')
        f.write('    (int)(sizeof(title_db) / sizeof(title_db[0]));\n')

    print(f'Wrote {len(sorted_entries)} total entries to {args.output}')


if __name__ == '__main__':
    main()
