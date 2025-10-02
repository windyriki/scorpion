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
from custom_parser import CommonParser
from labreports import PerTaskComparison

USER = project.dfsplan

BUILD = [] #debug
GENERATION_TIME = 100
REVISION_CACHE = project.DIR / "data" / "revision-cache"
REPO = project.get_repo_base()
MANUAL_DEBUG = False

if project.REMOTE:
    if MANUAL_DEBUG:
        ENV = TetralithEnvironment(
            email="windy.phung@liu.se",
            extra_options="#SBATCH -A naiss2025-5-382",
            memory_per_cpu="9G",
        )
        HOURS = 0
        MIN = 30
        TIME_LIMIT = int(HOURS * 60 + MIN)
        MEMORY_LIMIT = "8G"
        SUITE = project.SUITE_OPTIMAL_STRIPS_DEBUG
        BUILD += ["-j4"]
    else:
        ENV = TetralithEnvironment(
            email="windy.phung@liu.se",
            extra_options="#SBATCH -A naiss2025-5-382",
            memory_per_cpu="9G",
        )
        HOURS = 0
        MIN = 30
        TIME_LIMIT = int(HOURS * 60 + MIN)
        MEMORY_LIMIT = "8G"
        SUITE = build_suite(project.DOMAINS_DIR, project.SUITE_OPTIMAL_STRIPS)
        BUILD += ["-j4"]
else:
    if MANUAL_DEBUG:
        ENV = LocalEnvironment(processes=5)
        HOURS = 0
        MIN = 30
        TIME_LIMIT = int(HOURS * 60 + MIN)
        MEMORY_LIMIT = "8G"
        SUITE = project.SUITE_OPTIMAL_STRIPS_DEBUG_TINY
        GENERATION_TIME = 10
        BUILD += ["-j3"] # core angabe
    else:
        ENV = LocalEnvironment(processes=5)
        HOURS = 0
        MIN = 1
        TIME_LIMIT = int(HOURS * 60 + MIN)
        MEMORY_LIMIT = "3G"
        SUITE = project.SUITE_OPTIMAL_STRIPS_DEBUG
        GENERATION_TIME = 10
        BUILD += ["-j8"] # core angabe

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


GIT_REV_WLR = "348c4317450c36094585ec9cbc9ef22b9571e03f"
GIT_REV_WOLR = "348c4317450c36094585ec9cbc9ef22b9571e03f"
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

if MANUAL_DEBUG:
    MAX_PATTERN_SIZE_VALUES = [2,4,6]
else:
    MAX_PATTERN_SIZE_VALUES = [2,3,4,5,6,7,8,9,10]

for max_pattern_size_value in MAX_PATTERN_SIZE_VALUES:
    exp.add_algorithm(
        f"ppc (max_pattern_size={max_pattern_size_value})",
        project.SCORPION_DIR,
        GIT_REV_WLR,
        [
            "--translate-options",
            "--invariant-generation-max-candidates",
            "0", 
            "--search-options",
            "--search",
            f"""astar(pho(abstractions=[projections(sys_scp(max_pattern_size={max_pattern_size_value},
            max_pdb_size=infinity, max_collection_size=100M, max_patterns=infinity, max_time=15m,
            max_time_per_restart=infinity, saturate=false, pattern_type=interesting_general,
            ignore_useless_patterns=false, store_dead_ends=false))],
            max_orders=1,samples=1,saturated=true,ppc=true, max_optimization_time=0,diversify=false,
            output_file="test"),bound=0)"""
            # output_file="{task_name_safe}"),bound=0)"""
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
    "search_start_time",
    "search_start_memory"
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
