import json
import datetime
import argparse # Import the argparse library
from collections import defaultdict

def create_report(properties_file, output_file='report.html'):
    """
    Reads a JSON properties file, selects the best algorithm run for each
    problem, and generates an HTML report.
    """
    # --- 1. Read and Parse the JSON Data ---
    try:
        with open(properties_file, 'r') as f:
            properties = json.load(f)
    except FileNotFoundError:
        print(f"Error: The file '{properties_file}' was not found.")
        return
    except json.JSONDecodeError:
        print(f"Error: Could not decode JSON from '{properties_file}'.")
        return

    # --- 2. Group Runs by Domain and Problem ---
    grouped_runs = defaultdict(lambda: defaultdict(list))
    for run_data in properties.values():
        if 'id' in run_data and len(run_data['id']) >= 3:
            domain = run_data['id'][1]
            problem = run_data['id'][2]
            grouped_runs[domain][problem].append(run_data)

    # --- 3. Select the Best Run for Each Problem ---
    final_selection = defaultdict(list)
    for domain, problems in grouped_runs.items():
        for problem, runs in problems.items():
            best_run = sorted(
                runs,
                key=lambda r: (
                    r.get('max_pattern_size_in_ppc', -1),
                    -r.get('max_pattern_size', float('inf'))
                ),
                reverse=True
            )[0]
            final_selection[domain].append(best_run)
        final_selection[domain].sort(key=lambda r: r.get('problem', ''))

    # --- 4. Generate the HTML Report ---
    generate_html(final_selection, output_file)


def generate_html(data, output_file):
    """Generates the HTML file from the processed data."""
    columns = [
        'problem', 'error', 'max_pattern_size', 'max_pattern_size_in_ppc',
        'number_abstract_states', 'number_patterns_in_ppc', 'run_dir'
    ]
    html = f"""
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Taskwise Report</title>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; line-height: 1.6; }}
        table {{ border-collapse: collapse; width: 100%; margin: 20px 0; }}
        th, td {{ border: 1px solid #ddd; text-align: left; padding: 8px; }}
        th {{ background-color: #f2f2f2; font-weight: bold; }}
        h1, h2 {{ color: #333; border-bottom: 2px solid #f2f2f2; padding-bottom: 10px;}}
        h1 {{ font-size: 2em; }}
        h2 {{ font-size: 1.5em; margin-top: 40px; }}
    </style>
</head>
<body>
    <h1>taskwisereport</h1>
    <h2>{datetime.date.today().strftime('%Y-%m-%d')}</h2>
"""
    for domain in sorted(data.keys()):
        html += f"<h2>{domain}</h2>\n<table>\n"
        html += "  <thead>\n    <tr>\n"
        for col_name in columns:
            html += f"      <th>{col_name}</th>\n"
        html += "    </tr>\n  </thead>\n"
        html += "  <tbody>\n"
        for run in data[domain]:
            html += "    <tr>\n"
            for col_name in columns:
                value = run.get(col_name, 'None')
                html += f"      <td>{value}</td>\n"
            html += "    </tr>\n"
        html += "  </tbody>\n</table>\n"
    html += "</body>\n</html>"
    
    with open(output_file, 'w') as f:
        f.write(html)
    print(f"✅ Report successfully generated: {output_file}")


if __name__ == "__main__":
    # --- NEW: Set up command-line argument parsing ---
    parser = argparse.ArgumentParser(
        description="Generate an HTML report from a properties JSON file."
    )
    # Add an argument for the input file path
    parser.add_argument(
        "input_file",
        help="Path to the input properties JSON file."
    )
    # Add an optional argument for the output file path
    parser.add_argument(
        "-o", "--output",
        default="report.html",
        help="Path to the output HTML file (default: report.html)"
    )
    
    args = parser.parse_args()
    
    # Call the main function with the provided file paths
    create_report(properties_file=args.input_file, output_file=args.output)