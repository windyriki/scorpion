#! /usr/bin/env python

from downward.experiment import FastDownwardExperiment
from downward.suites import build_suite
from lab.environments import TetralithEnvironment, LocalEnvironment
from lab.reports import Attribute, geometric_mean, arithmetic_mean
from downward.reports.absolute import AbsoluteReport
from downward.reports.taskwise import TaskwiseReport
from downward.cached_revision import CachedFastDownwardRevision
from downward.reports.compare import ComparativeReport
from downward.reports.scatter import ScatterPlotReport
from downward.experiment import FastDownwardAlgorithm, FastDownwardRun
from lab.experiment import Experiment
from downward import suites
import project
from project import report_names
import shutil
from pathlib import Path
from functools import partial
import json
import subprocess
from custom_parser import CommonParser
from labreports import PerTaskComparison

USER = project.dfsplan

BUILD = [] #debug
GENERATION_TIME = 100
REVISION_CACHE = project.DIR / "data" / "revision-cache"
REPO = project.get_repo_base()

if project.REMOTE:
    ENV = TetralithEnvironment(
        email="windy.phung@liu.se",
        extra_options="#SBATCH -A naiss2025-5-382",
        memory_per_cpu="9G",
        cpus_per_task=4,
    )
    HOURS = 1
    MIN = 0
    TIME_LIMIT = int(HOURS * 60 + MIN)
    MEMORY_LIMIT = "32G"
    # SUITE = build_suite(project.DOMAINS_DIR, project.SUITE_OPTIMAL_STRIPS)
    SUITE = project.SUITE_OPTIMAL_STRIPS_DEBUG_GRIPPER
    BUILD += ["-j4"]
else:
    ENV = LocalEnvironment(processes=5)
    HOURS = 0
    MIN = 30
    TIME_LIMIT = int(HOURS * 60 + MIN)
    MEMORY_LIMIT = "12G"
    SUITE = project.SUITE_OPTIMAL_STRIPS_DEBUG_TINY
    GENERATION_TIME = 10
    BUILD += ["-j4"] # core angabe

DRIVER = [
    "--overall-time-limit",
    f"{TIME_LIMIT}m",
    "--overall-memory-limit",
    MEMORY_LIMIT
    # "--debug"
]


def no_search(run):
    if "search_start_time" not in run:
        error = run.get("error")
        if error is not None and error != "incomplete-search-found-no-plan":
            run["error"] = "no-search-due-to-" + error
    return run


def add_search_started(run):
    run["search_started"] = run.get("search_start_time") is not None
    return run


GIT_REV_WLR = "6dccd392ac791fdb617e8567a6ebbc31055bb8bb"
GIT_REV_WOLR = "6dccd392ac791fdb617e8567a6ebbc31055bb8bb"
exp = FastDownwardExperiment(environment=ENV)
exp.add_parser(FastDownwardExperiment.EXITCODE_PARSER)
exp.add_parser(FastDownwardExperiment.TRANSLATOR_PARSER)
exp.add_parser(FastDownwardExperiment.SINGLE_SEARCH_PARSER)
exp.add_parser(CommonParser())


exp.add_resource("", "project.py")

# abstractions = (
#     "[cartesian(subtasks=[goals(order=random,random_seed=5555)],random_seed=5555)]"
# ) random seed important if random orders

# for task in SUITE:
# task_name_safe = Path(task).stem.replace(":", "_")
# Path(task_name_safe).mkdir(parents=True, exist_ok=True)

exp.add_algorithm(
    f"ppc (max_pattern_size=maximal)",
    project.SCORPION_DIR,
    GIT_REV_WLR,
    [
        "--translate-options",
        "--invariant-generation-max-candidates",
        "0", 
        "--dump-static-atoms",
        "--search-options",
        "--search",
        f"""astar(pho(abstractions=[projections(sys_scp(max_pattern_size=-1,
        max_pdb_size=infinity, max_collection_size=100M, max_patterns=infinity, max_time=15m,
        max_time_per_restart=infinity, saturate=false, pattern_type=interesting_general,
        ignore_useless_patterns=false, store_dead_ends=false))],
        max_orders=1,samples=1,saturated=true,ppc=true, max_optimization_time=0,diversify=false,
        output_file="test_maximal"),bound=0)"""
    ],
    build_options=BUILD,
    driver_options=DRIVER,
)
exp.add_algorithm(
    f"ppc (max_pattern_size=4)",
    project.SCORPION_DIR,
    GIT_REV_WLR,
    [
        "--translate-options",
        "--invariant-generation-max-candidates",
        "0", 
        "--dump-static-atoms",
        "--search-options",
        "--search",
        f"""astar(pho(abstractions=[projections(sys_scp(max_pattern_size=4,
        max_pdb_size=infinity, max_collection_size=100M, max_patterns=infinity, max_time=15m,
        max_time_per_restart=infinity, saturate=false, pattern_type=interesting_general,
        ignore_useless_patterns=false, store_dead_ends=false))],
        max_orders=1,samples=1,saturated=true,ppc=true, max_optimization_time=0,diversify=false,
        output_file="test_4"),bound=0)"""
    ],
    build_options=BUILD,
    driver_options=DRIVER,
)

# MIN_OPS_PER_LABEL_VALUES = [0, 2]
# MIN_OCCURRENCES_PER_LABEL_VALUES = [1, 2, 5, 10, 20, 50]

# for min_ops in MIN_OPS_PER_LABEL_VALUES:
#     for min_occurrences in MIN_OCCURRENCES_PER_LABEL_VALUES:
#         exp.add_algorithm(
#             f"with label reduction (min_ops_per_label={min_ops}, min_occurrences_per_label={min_occurrences})",
#             project.SCORPION_DIR,
#             GIT_REV_WLR,
#             [
#                 "--search",
#                 f"""astar(scp([cartesian(subtasks=[landmarks(order=random,random_seed=0)],random_seed=0, 
#                 min_ops_per_label={min_ops}, min_occurrences_per_label={min_occurrences}),
#                 cartesian(subtasks=[goals(order=random,random_seed=0)], min_ops_per_label={min_ops},
#                 min_occurrences_per_label={min_occurrences}),
#                 projections(systematic(2), create_complete_transition_system=true, min_ops_per_label={min_ops}, 
#                 min_occurrences_per_label={min_occurrences})],
#                 max_orders=1K, diversify=false, max_time=infinity, max_optimization_time=0))""",
#             ],
#             build_options=BUILD,
#             driver_options=DRIVER,
#         )

exp.add_suite(project.DOMAINS_DIR, SUITE)

# Add step that writes experiment files to disk.
exp.add_step("build", exp.build)

# Add step that executes all runs.
exp.add_step("start", exp.start_runs)

exp.add_step("parse", exp.parse)

# Add custom step to process training data for each problem
def process_training_data(mode="other"):
    """Call add_static_and_other_atoms.py or add_static_and_all_atoms.py for each run directory.
    
    Args:
        mode: Either "other" or "all" to choose which script to use.
    """
    if mode == "all":
        script_path = REPO / "add_static_and_all_atoms.py"
        output_suffix = "all"
    else:
        script_path = REPO / "add_static_and_other_atoms.py"
        output_suffix = "other"
    
    # Get run directories from the experiment path
    exp_path = Path(exp.path)
    if not exp_path.exists():
        print(f"Experiment directory {exp_path} does not exist. Run steps 1-3 first.")
        return
    
    # Find all run directories
    run_dirs = [d for d in exp_path.iterdir() if d.is_dir() and d.name.startswith("runs-")]
    if run_dirs:
        run_dirs = [d for runs_dir in run_dirs for d in runs_dir.iterdir() if d.is_dir()]
    else:
        # Alternative structure: runs directly in exp directory
        run_dirs = [d for d in exp_path.iterdir() if d.is_dir() and not d.name.startswith(".")]
    
    for run_dir in run_dirs:
        if not run_dir.exists():
            continue
            
        # Check if required files exist in run directory
        test_mapping = run_dir / "test_mapping.csv"
        static_atoms = run_dir / "static-atoms.txt"
        training_data = run_dir / "test_ppc_training_data.csv"
        
        if training_data.exists():
            output_file = run_dir / f"test_ppc_training_data_updated_{output_suffix}.csv"
            
            # Build command
            cmd = [
                "python3", str(script_path),
                "-i", str(training_data),
                "-o", str(output_file),
            ]
            
            # Add optional file paths if they exist
            if static_atoms.exists():
                cmd.extend(["-s", str(static_atoms)])
            if test_mapping.exists():
                cmd.extend(["-m", str(test_mapping)])
            
            # Run the script
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, check=True)
                print(f"Processed {run_dir.name}: {output_file}")
            except subprocess.CalledProcessError as e:
                print(f"Error processing {run_dir.name}: {e}")
                print(f"stdout: {e.stdout}")
                print(f"stderr: {e.stderr}")

exp.add_step("process-training-data-other", lambda: process_training_data("other"))
exp.add_step("process-training-data-all", lambda: process_training_data("all"))

# Add step that collects properties from run directories and
# writes them to *-eval/properties.
exp.add_fetcher(name="fetch")

ATTRIBUTES = [ #schaue mal durch
    "error",
    "run_dir",
    "total_time",
    "coverage",
    "memory",
    "evaluations",
    "expansions",
    "expansions_until_last_jump",
    # "h_values",
    "search_time",
    "max_pattern_size",
    "max_pattern_size_in_ppc",
    "number_patterns_in_ppc",
    "number_abstract_states",
    "number_abstract_states_stored_values",
    "search_start_time",
    "search_start_memory",
    "translator_variables",
]

# exp.add_report(TaskwiseReport(attributes=["run_dir","max_pattern_size", "max_pattern_size_in_ppc", "number_patterns_in_ppc", "number_abstract_states", "error"])),
project.add_report(
    exp,
    attributes=ATTRIBUTES,
    filter=[
    ],
)

if project.REMOTE:
    project.add_compress_exp_dir_step(exp)
else:
    project.add_scp_step(exp, USER.scp_login, USER.remote_repo)
    project.add_report(
        exp,
        attributes=ATTRIBUTES,
        name=f"{exp.name}-{report_names[AbsoluteReport]}-cluster",
        eval_dir=project.get_cluster_eval_dir(exp),
        filter=[
        ],
    )


# def rename_algorithm(renames, run):
#     name = run["algorithm"]
#     if name in renames:
#         run["algorithm"] = renames[name]
#     return run


# def domain_as_category(run1, run2):
#     # run2['domain'] has the same value, because we always
#     # compare two runs of the same problem.
#     return run1["domain"]


# Parse the commandline and run the given steps.
exp.run_steps()
