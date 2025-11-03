#!/usr/bin/env python3
"""
Add static_atoms and other_atoms columns to the training data CSV.
- static_atoms: atoms from static-atoms.txt
- other_atoms: atoms in test_mapping.csv that are not in (static + state + goal)
"""

import argparse
import csv
from pathlib import Path

# Parse command-line arguments
parser = argparse.ArgumentParser(description='Add static_atoms and other_atoms columns to training data CSV')
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
all_atoms = []
if Path(mapping_file).exists():
    with open(mapping_file, 'r', encoding='utf-8') as f:
        reader = csv.reader(f, delimiter=';')
        next(reader)  # Skip header
        for row in reader:
            if len(row) >= 2:
                atom = row[1].strip()
                all_atoms.append(atom)
    print(f"Loaded {len(all_atoms)} total atoms from mapping")
else:
    print(f"Warning: {mapping_file} not found")

# Read and process training data
with open(training_data_file, 'r', encoding='utf-8') as f_in:
    reader = csv.reader(f_in, delimiter=';')
    header = next(reader)
    
    with open(output_file, 'w', encoding='utf-8', newline='') as f_out:
        writer = csv.writer(f_out, delimiter=';')
        
        # Write new header
        new_header = ['state_atoms', 'goal_atoms', 'static_atoms', 'other_atoms', 'perfect_pattern_collection']
        writer.writerow(new_header)
        
        row_count = 0
        for row in reader:
            if len(row) < 3:
                print(f"Skipping malformed row: {row}")
                continue
            
            state_facts = row[0].strip()
            goal_facts = row[1].strip()
            ppc = row[2].strip()
            
            # Parse state facts into set
            state_atoms_set = set()
            if state_facts:
                # Split by comma, being careful with commas inside parentheses
                atoms_list = []
                current_atom = ""
                paren_depth = 0
                for char in state_facts:
                    if char == '(':
                        paren_depth += 1
                        current_atom += char
                    elif char == ')':
                        paren_depth -= 1
                        current_atom += char
                    elif char == ',' and paren_depth == 0:
                        atom_stripped = current_atom.strip()
                        if atom_stripped:
                            atoms_list.append(atom_stripped)
                        current_atom = ""
                    else:
                        current_atom += char
                if current_atom.strip():
                    atoms_list.append(current_atom.strip())
                state_atoms_set = set(atoms_list)
            
            # Parse goal facts into set
            goal_atoms_set = set()
            if goal_facts:
                atoms_list = []
                current_atom = ""
                paren_depth = 0
                for char in goal_facts:
                    if char == '(':
                        paren_depth += 1
                        current_atom += char
                    elif char == ')':
                        paren_depth -= 1
                        current_atom += char
                    elif char == ',' and paren_depth == 0:
                        atom_stripped = current_atom.strip()
                        if atom_stripped:
                            atoms_list.append(atom_stripped)
                        current_atom = ""
                    else:
                        current_atom += char
                if current_atom.strip():
                    atoms_list.append(current_atom.strip())
                goal_atoms_set = set(atoms_list)
            
            # Calculate other atoms: all_atoms - (static + state + goal)
            used_atoms = static_atoms | state_atoms_set | goal_atoms_set
            other_atoms_set = set()
            for atom in all_atoms:
                if atom not in used_atoms:
                    other_atoms_set.add(atom)
            
            # Format outputs - remove spaces after commas in atoms
            def clean_atom(atom):
                return atom.replace(", ", ",")
            
            static_atoms_str = ",".join(clean_atom(a) for a in sorted(static_atoms)) if static_atoms else ""
            other_atoms_str = ",".join(clean_atom(a) for a in sorted(other_atoms_set)) if other_atoms_set else ""
            
            # Clean state_facts and goal_facts
            state_facts_clean = state_facts.replace(", ", ",")
            goal_facts_clean = goal_facts.replace(", ", ",")
            
            # Write row
            writer.writerow([state_facts_clean, goal_facts_clean, static_atoms_str, other_atoms_str, ppc])
            row_count += 1

print(f"\nProcessed {row_count} rows")
print(f"Output written to: {output_file}")
print(f"\nSample counts:")
print(f"  Static atoms: {len(static_atoms)}")
print(f"  Total atoms in mapping: {len(all_atoms)}")
