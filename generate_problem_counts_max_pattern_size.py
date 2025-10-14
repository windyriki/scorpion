from bs4 import BeautifulSoup
import pandas as pd
import os
from io import StringIO
from typing import Dict, Any
import re

# --- CONFIGURATION ---
INPUT_FILE_PATH = "experiments/atd/report_cluster.html"
OUTPUT_FILE_1 = "max_pattern_size_per_domain.html"
OUTPUT_FILE_2 = "max_pattern_size_in_ppc_per_domain.html"
COLUMN_1 = "max_pattern_size"
COLUMN_2 = "max_pattern_size_in_ppc"

# --- FILE READING WITH MOCK FALLBACK ---

try:
    # Attempt to load the user's input file
    with open(INPUT_FILE_PATH, "r", encoding="utf-8") as f:
        soup = BeautifulSoup(f, "html.parser")
    print(f"✅ Successfully loaded input file: {INPUT_FILE_PATH}")

except FileNotFoundError:
    # --- MOCK DATA FOR DEMONSTRATION ---
    print(f"⚠️ Input file not found at '{INPUT_FILE_PATH}'. Using mock data for demonstration.")
    # The mock data structure now includes both required columns for testing both reports
    MOCK_HTML_CONTENT = """
    <html><body>
        <h2>Domain X</h2><table><thead><tr><th>problem_id</th><th>max_pattern_size</th><th>max_pattern_size_in_ppc</th></tr></thead><tbody>
            <tr><td>p1</td><td>1</td><td>4</td></tr><tr><td>p2</td><td>2</td><td>4</td></tr>
            <tr><td>p3</td><td>2</td><td>5</td></tr><tr><td>p4</td><td>1</td><td>4</td></tr>
        </tbody></table>
        <h2>Domain Y</h2><table><thead><tr><th>problem_id</th><th>max_pattern_size</th><th>max_pattern_size_in_ppc</th></tr></thead><tbody>
            <tr><td>q1</td><td>2</td><td>6</td></tr><tr><td>q2</td><td>3</td><td>4</td></tr>
            <tr><td>q3</td><td>2</td><td>6</td></tr><tr><td>q4</td><td>2</td><td>6</td></tr>
            <tr><td>q5</td><td>1</td><td>4</td></tr>
        </tbody></table>
        <h2>Domain Z</h2><table><thead><tr><th>problem_id</th><th>max_pattern_size</th><th>max_pattern_size_in_ppc</th></tr></thead><tbody>
            <tr><td>r1</td><td>3</td><td>7</td></tr><tr><td>r2</td><td>1</td><td>5</td></tr><tr><td>r3</td><td>1</td><td>5</td></tr>
        </tbody></table>
    </body></html>
    """
    soup = BeautifulSoup(StringIO(MOCK_HTML_CONTENT), "html.parser")
except Exception as e:
    print(f"❌ An error occurred while reading the file: {e}")
    exit()

# --- REUSABLE FUNCTIONS ---

def generate_summary_df(soup: BeautifulSoup, count_column: str) -> pd.DataFrame:
    """Processes the parsed HTML to create a summary DataFrame based on a given column."""
    
    # Walk through each domain header and pair it with the next table element.
    # This avoids misalignment when the document contains extra tables (e.g. overall summaries).
    data: list[Dict[Any, Any]] = []
    for h2 in soup.find_all("h2"):
        domain = h2.text
        # Filter out non-domain h2 tags (like date stamps or report titles)
        if domain.isupper() or domain in ["2025-10-03", "taskwisereport"]:
            continue
        table = h2.find_next("table")
        if table is None:
            continue
        df = pd.read_html(str(table))[0]
        if count_column in df.columns:
            # Ensure the count column is integer-typed so pandas produces integer
            # column labels (avoid floats like 3.0 when values were parsed as floats).
            try:
                # Convert to numeric. For the PPC column we want to treat missing values
                # as 0 (count them as size 0). For other columns, drop missing values.
                df[count_column] = pd.to_numeric(df[count_column], errors='coerce')
                if count_column == COLUMN_2:
                    # Replace NaN/None with 0 so they are counted as size 0
                    df[count_column] = df[count_column].fillna(0).astype(int)
                else:
                    # For other columns, drop rows with non-numeric / missing values
                    df = df.dropna(subset=[count_column])
                    df[count_column] = df[count_column].astype(int)
            except Exception:
                pass
            counts = df[count_column].value_counts().to_dict()
            counts["domain"] = domain
            counts["total"] = len(df)
            data.append(counts)

    summary_df = pd.DataFrame(data).fillna(0).set_index("domain")

    # Sort numeric columns cleanly (e.g., sort '1', '2', '10' correctly)
    cols_numeric = [c for c in summary_df.columns if isinstance(c, (int, float)) or str(c).isdigit()]
    cols_others = [c for c in summary_df.columns if c not in cols_numeric]
    sorted_cols = sorted(cols_numeric, key=lambda x: int(x)) + cols_others
    summary_df = summary_df[sorted_cols].astype(int)

    # Store total before dropping the column for the index update
    domain_totals = summary_df['total'].copy()

    # Normalize domain labels: remove any trailing parenthetical (e.g. '(20 problems)')
    # and append the canonical ' (N)' where N is the computed total.
    def clean_domain_label(domain_label: str) -> str:
        # Remove any trailing parenthetical like ' (...)'
        cleaned = re.sub(r"\s*\([^)]*\)\s*$", "", domain_label).strip()
        return cleaned

    summary_df.index = [f"{clean_domain_label(d)} ({int(domain_totals.loc[d])})" for d in summary_df.index]

    # Remove the 'total' column from the data shown
    summary_df = summary_df.drop(columns=["total"])

    # Add Summation Row
    sum_row = summary_df.sum().to_frame().T
    sum_row.index = ["Total"]
    summary_df = pd.concat([summary_df, sum_row])

    return summary_df

def save_report(summary_df: pd.DataFrame, count_column: str, output_filename: str):
    """Generates the final HTML structure and saves it to the specified file."""
    
    html_title = f"Problem Counts by Domain and {count_column}"
    
    html_table = summary_df.to_html(
        index=True,
        classes='dataframe',
        border=0,
    )

    # Insert the HTML table into the full document structure with styling
    html_output = f"""
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{html_title}</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f9; }}
        .container {{ max-width: 900px; margin: 0 auto; background-color: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }}
        h1 {{ color: #004d99; border-bottom: 2px solid #004d99; padding-bottom: 10px; }}
        p {{ color: #555; }}
        table.dataframe {{
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
            font-size: 0.95em;
            text-align: center;
        }}
        table.dataframe th, table.dataframe td {{
            padding: 12px 15px;
            border: 1px solid #ddd;
        }}
        table.dataframe thead th {{
            background-color: #e6f2ff;
            color: #333;
            font-weight: bold;
            text-transform: uppercase;
        }}
        table.dataframe tbody th {{
            text-align: left;
            background-color: #f9f9f9;
            font-weight: normal;
        }}
        table.dataframe tbody tr:nth-child(even) {{
            background-color: #f0f8ff;
        }}
        /* Highlight the 'Total' row */
        table.dataframe tbody tr:last-child th,
        table.dataframe tbody tr:last-child td {{
            background-color: #ffe0b2;
            font-weight: bold;
            border-top: 3px double #333;
        }}
        table.dataframe tbody tr:hover {{
            background-color: #e0f0ff;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>{html_title}</h1>
        <p>This report summarizes the count of problems per domain, categorized by their **{count_column}**. The total number of problems for each domain is indicated in parentheses next to the domain name.</p>
        {html_table}
        <p style="margin-top: 30px; font-size: 0.8em; color: #999;">Generated by Python and pandas.</p>
    </div>
</body>
</html>
"""

    # === SAVE TO FILE ===
    try:
        with open(output_filename, "w", encoding="utf-8") as f_out:
            f_out.write(html_output)
        print(f"✅ Report saved: {os.path.abspath(output_filename)}")
    except IOError as e:
        print(f"❌ Error writing to file {output_filename}: {e}")

# --- MAIN EXECUTION ---

print("\n--- Starting Report Generation ---")

# 1. Generate Report for max_pattern_size
summary_df_1 = generate_summary_df(soup, COLUMN_1)
save_report(summary_df_1, COLUMN_1, OUTPUT_FILE_1)

print("\n---")

# 2. Generate Report for max_pattern_size_in_ppc
summary_df_2 = generate_summary_df(soup, COLUMN_2)
save_report(summary_df_2, COLUMN_2, OUTPUT_FILE_2)

print("\n--- Report Generation Complete ---\n")
