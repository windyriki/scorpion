"""
Scan the workspace for files matching '*_ppc_training_data.csv'.
For each file, parse the 'perfect_pattern_collection' field and check whether any pattern
in the collection has length > 3. If yes, copy that CSV and its corresponding
_mapping.csv (if present) into 'training_data_>3/'.

Usage: python copy_training_data_gt3.py
"""
import os
import re
import shutil
import csv

ROOT = os.getcwd()
# Directory containing the original training data to scan
SEARCH_ROOT = os.path.join(ROOT, "experiments/atd/training_data")
OUT_DIR = os.path.join(ROOT, "training_data_>3")
PATTERN = re.compile(r"\[([^\[\]]*)\]")  # matches non-nested [..] groups


def pattern_lengths_from_field(field: str):
    # field is expected like: "[[1, 2], [3, 4, 5]]" or "[]"
    if not field:
        return []
    # find inner bracket groups (non-nested)
    matches = PATTERN.findall(field)
    lengths = []
    for m in matches:
        # We need to split top-level commas only (ignore commas inside parentheses).
        parts = []
        buf = []
        depth = 0
        for ch in m:
            if ch == '(':
                depth += 1
                buf.append(ch)
            elif ch == ')':
                if depth > 0:
                    depth -= 1
                buf.append(ch)
            elif ch == ',' and depth == 0:
                token = ''.join(buf).strip()
                if token:
                    parts.append(token)
                buf = []
            else:
                buf.append(ch)
        # flush
        token = ''.join(buf).strip()
        if token:
            parts.append(token)
        # remove empty tokens
        parts = [p for p in parts if p]
        lengths.append(len(parts))
    return lengths


if __name__ == '__main__':
    os.makedirs(OUT_DIR, exist_ok=True)
    found = []
    # Walk only the training_data folder
    for dirpath, dirnames, filenames in os.walk(SEARCH_ROOT):
        for fname in filenames:
            if not fname.lower().endswith('.csv'):
                continue
            full = os.path.join(dirpath, fname)
            with open(full, newline='') as csvfile:
                reader = csv.reader(csvfile, delimiter=';')
                headers = next(reader, None)
                if not headers:
                    continue
                # normalize header names and find the column with perfect_pattern_collection
                header_names = [h.strip().lower() for h in headers]
                try:
                    col_index = next(i for i, h in enumerate(header_names) if 'perfect_pattern_collection' in h)
                except StopIteration:
                    # no such column in this CSV
                    continue
                # read rows and check the pattern collection column
                for row in reader:
                    if len(row) <= col_index:
                        field = ''
                    else:
                        field = row[col_index].strip()
                        lengths = pattern_lengths_from_field(field)
                        if any(l > 3 for l in lengths):
                            # Preserve directory structure: compute relative path from SEARCH_ROOT
                            rel_dir = os.path.relpath(dirpath, SEARCH_ROOT)
                            dst_dir = os.path.join(OUT_DIR, rel_dir)
                            os.makedirs(dst_dir, exist_ok=True)
                            dst = os.path.join(dst_dir, fname)
                            shutil.copy2(full, dst)
                            mapping = full.replace('_ppc_training_data.csv', '_mapping.csv')
                            if os.path.exists(mapping):
                                shutil.copy2(mapping, os.path.join(dst_dir, os.path.basename(mapping)))
                            found.append(full)
                            break
    print(f"Copied {len(found)} files to {OUT_DIR}")
