import json
import datetime
import argparse # Import the argparse library
from collections import defaultdict
import math # Import math for initial min/max values

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

    # --- 3. Select the Best Run for Each Problem and Aggregate Stats ---
    final_selection = defaultdict(list)
    domain_summary = defaultdict(int) 
    problems_per_domain = defaultdict(int)
    ppc_gt_3_count = defaultdict(int)
    
    domain_stats = defaultdict(lambda: {
        'abstract_states_min': math.inf, 'abstract_states_max': -math.inf,
        'patterns_ppc_min': math.inf, 'patterns_ppc_max': -math.inf,
        'translator_vars_min': math.inf, 'translator_vars_max': -math.inf,
        # --- Tracking for max_pattern_size_in_ppc range ---
        'ppc_size_min': math.inf, 'ppc_size_max': -math.inf,
        # --- Tracking for max_pattern_size range ---
        'max_p_size_min': math.inf, 'max_p_size_max': -math.inf,
    })
    
    metrics = [
        ('number_abstract_states', 'abstract_states'),
        ('number_patterns_in_ppc', 'patterns_ppc'),
        ('translator_variables', 'translator_vars'),
        ('max_pattern_size_in_ppc', 'ppc_size'),
        ('max_pattern_size', 'max_p_size')
    ]

    for domain, problems in grouped_runs.items():
        problems_per_domain[domain] = len(problems)
        
        for problem, runs in problems.items():
            best_run = sorted(
                runs,
                key=lambda r: (
                    r.get('max_pattern_size_in_ppc', -1),
                    r.get('max_pattern_size', -1)
                ),
                reverse=True
            )[0]
            final_selection[domain].append(best_run)
            
            # Update summary count for the best run
            max_p_size = best_run.get('max_pattern_size')
            max_p_size_ppc = best_run.get('max_pattern_size_in_ppc')
            
            # Check for equality
            if max_p_size is not None and max_p_size_ppc is not None and max_p_size == max_p_size_ppc:
                domain_summary[domain] += 1
            
            # Check for max_pattern_size_in_ppc > 3
            if max_p_size_ppc is not None:
                try:
                    if int(max_p_size_ppc) > 3:
                        ppc_gt_3_count[domain] += 1
                except (ValueError, TypeError):
                    pass

            # Update Min/Max Stats for the domain
            for key, stat_name in metrics:
                value = best_run.get(key)
                if value is not None:
                    try:
                        value = int(value) # Ensure the value is an integer for comparison
                        
                        # Update min
                        if value < domain_stats[domain][f'{stat_name}_min']:
                            domain_stats[domain][f'{stat_name}_min'] = value
                        # Update max
                        if value > domain_stats[domain][f'{stat_name}_max']:
                            domain_stats[domain][f'{stat_name}_max'] = value
                    except (ValueError, TypeError):
                        # Skip if value can't be converted to int
                        continue
                
        final_selection[domain].sort(key=lambda r: r.get('problem', ''))

    # --- 4. Generate the HTML Report ---
    generate_html(final_selection, output_file, domain_summary, problems_per_domain, domain_stats, ppc_gt_3_count)


def generate_html(data, output_file, domain_summary, problems_per_domain, domain_stats, ppc_gt_3_count):
    """Generates the HTML file from the processed data."""
    columns = [
        'problem', 'error', 'max_pattern_size', 'max_pattern_size_in_ppc',
        'number_abstract_states', 'number_patterns_in_ppc', 'translator_variables', 'run_dir'
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
        h1, h2, h3 {{ color: #333; border-bottom: 2px solid #f2f2f2; padding-bottom: 10px;}}
        h1 {{ font-size: 2em; }}
        h2 {{ font-size: 1.5em; margin-top: 40px; }}
        h3 {{ font-size: 1.2em; margin-top: 20px; border-bottom: none; }}
        .summary-table {{ width: auto; }}
        .summary-table th, .summary-table td {{ text-align: center; }}
    </style>
</head>
<body>
    <h1>taskwisereport</h1>
    <h2>{datetime.date.today().strftime('%Y-%m-%d')}</h2>

    <h2>Domain Summary</h2>
    <table class="summary-table">
      <thead>
        <tr>
          <th>Domain</th>
          <th>max_pattern_size == max_pattern_size_in_ppc</th>
          <th>max_pattern_size > max_pattern_size_in_ppc</th>
          <th>max_pattern_size_in_ppc > 3</th>
          <th>max_pattern_size (Min - Max)</th>
          <th>max_pattern_size_in_ppc (Min - Max)</th>
          <th>patterns_in_ppc (Min - Max)</th>
          <th>abstract_states (Min - Max)</th>
          <th>translator_variables (Min - Max)</th>
        </tr>
      </thead>
      <tbody>
"""
    # Populate the summary table
    for domain in sorted(problems_per_domain.keys()):
        total_problems = problems_per_domain[domain]
        match_count = domain_summary.get(domain, 0)
        non_match_count = total_problems - match_count
        gt_3_count = ppc_gt_3_count.get(domain, 0)
        
        stats = domain_stats[domain]
        
        # Helper to format min/max, handling potential empty domains (where min=inf, max=-inf)
        def format_range(min_val, max_val):
            if min_val == math.inf or max_val == -math.inf:
                return "N/A"
            return f"{min_val} - {max_val}"
            
        abstract_states_range = format_range(stats['abstract_states_min'], stats['abstract_states_max'])
        patterns_ppc_range = format_range(stats['patterns_ppc_min'], stats['patterns_ppc_max'])
        translator_vars_range = format_range(stats['translator_vars_min'], stats['translator_vars_max'])
        ppc_size_range = format_range(stats['ppc_size_min'], stats['ppc_size_max'])
        max_p_size_range = format_range(stats['max_p_size_min'], stats['max_p_size_max'])

        html += f"""
        <tr>
          <td>{domain} ({total_problems})</td>
          <td>{match_count}</td>
          <td>{non_match_count}</td>
          <td>{gt_3_count}</td>
          <td>{max_p_size_range}</td>
          <td>{ppc_size_range}</td>
          <td>{patterns_ppc_range}</td>
          <td>{abstract_states_range}</td>
          <td>{translator_vars_range}</td>
        </tr>
"""
    html += """
      </tbody>
    </table>
    """

    for domain in sorted(data.keys()):
        # Keep the count here as well for immediate context on the problem list
        problem_count = problems_per_domain.get(domain, 0)
        html += f"<h2>{domain} ({problem_count} problems)</h2>\n<table>\n"
        
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
    # Set up command-line argument parsing
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