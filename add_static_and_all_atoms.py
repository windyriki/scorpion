#!/usr/bin/env python3
"""
Add static_atoms and all_atoms columns to the training data CSV.
- static_atoms: atoms from static-atoms.txt
- all_atoms: all atoms in test_mapping.csv (first column)
"""

import argparse
import csv
from pathlib import Path

# Parse command-line arguments
parser = argparse.ArgumentParser(description='Add static_atoms and all_atoms columns to training data CSV')
parser.add_argument('-i', '--input', default='test_ppc_training_data.csv',
                    help='Input training data CSV file (default: test_ppc_training_data.csv)')
parser.add_argument('-o', '--output', default='test_ppc_training_data_updated.csv',
                    help='Output CSV file (default: test_ppc_training_data_updated.csv)')
parser.add_argument('-s', '--static', default='static-atoms.txt',
                    help='Static atoms file (default: static-atoms.txt)')
parser.add_argument('-m', '--mapping', default='test_mapping.csv',
                    help='Mapping CSV file (default: test_mapping.csv)')
args = parser.parse_args()

# File paths
training_data_file = args.input
static_atoms_file = args.static
mapping_file = args.mapping
output_file = args.output

# Read static atoms
static_atoms = set()
if Path(static_atoms_file).exists():
    with open(static_atoms_file, 'r', encoding='utf-8') as f:
        for line in f:
            atom = line.strip()
            if atom:
                # Remove "Atom " prefix if present
                if atom.startswith("Atom "):
                    atom = atom[5:]
                static_atoms.add(atom)
    print(f"Loaded {len(static_atoms)} static atoms")
else:
    print(f"Warning: {static_atoms_file} not found")

# Read all atoms from mapping file
mapping_atoms = set()
if Path(mapping_file).exists():
    with open(mapping_file, 'r', encoding='utf-8') as f:
        reader = csv.reader(f, delimiter=';')
        next(reader)  # Skip header
        for row in reader:
            if len(row) >= 2:
                atom = row[1].strip()
                mapping_atoms.add(atom)
    print(f"Loaded {len(mapping_atoms)} atoms from mapping")
else:
    print(f"Warning: {mapping_file} not found")

# Create union of mapping atoms and static atoms
all_atoms = mapping_atoms.union(static_atoms)
print(f"Total unique atoms (union): {len(all_atoms)}")

# Read and process training data
with open(training_data_file, 'r', encoding='utf-8') as f_in:
    reader = csv.reader(f_in, delimiter=';')
    header = next(reader)
    
    with open(output_file, 'w', encoding='utf-8', newline='') as f_out:
        writer = csv.writer(f_out, delimiter=';')
        # Write new header
        new_header = ['all_atoms', 'state_atoms', 'goal_atoms', 'static_atoms', 'perfect_pattern_collection']
        writer.writerow(new_header)
        row_count = 0
        for row in reader:
            if len(row) < 3:
                print(f"Skipping malformed row: {row}")
                continue
            state_facts = row[0].strip()
            goal_facts = row[1].strip()
            ppc = row[2].strip()
            # Format outputs - remove spaces after commas in atoms
            def clean_atom(atom):
                return atom.replace(", ", ",")
            all_atoms_str = ",".join(clean_atom(a) for a in all_atoms) if all_atoms else ""
            static_atoms_str = ",".join(clean_atom(a) for a in sorted(static_atoms)) if static_atoms else ""
            state_facts_clean = state_facts.replace(", ", ",")
            goal_facts_clean = goal_facts.replace(", ", ",")
            # Write row
            writer.writerow([all_atoms_str, state_facts_clean, goal_facts_clean, static_atoms_str, ppc])
            row_count += 1

print(f"\nProcessed {row_count} rows")
print(f"Output written to: {output_file}")
print(f"\nSample counts:")
print(f"  Static atoms: {len(static_atoms)}")
print(f"  Total atoms in mapping: {len(all_atoms)}")
