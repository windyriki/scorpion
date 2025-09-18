#include "pho_heuristic.h"

#include "abstraction.h"
#include "cost_partitioning_heuristic.h"
#include "cost_partitioning_heuristic_collection_generator.h"
#include "max_cost_partitioning_heuristic.h"
#include "uniform_cost_partitioning_heuristic.h"
#include "utils.h"

#include "../algorithms/partial_state_tree.h"
#include "../plugins/plugin.h"
#include "../task_utils/task_properties.h"
#include "../utils/logging.h"
#include "projection.h"
#include "../task_proxy.h"
#include "types.h"

using namespace std;

namespace cost_saturation {
/*
  The implementation currently computes weighted lookup tables for PhO and
  holds them in memory. A more efficient implementation would only store the
  weights and compute the weighted heuristic values on the fly when evaluating
  a state.
*/
PhO::PhO(
    const Abstractions &abstractions, const vector<int> &costs,
    lp::LPSolverType solver_type, bool saturated, const utils::LogProxy &log,
    std::shared_ptr<AbstractTask> task_ptr, bool ppc)
    : lp_solver(solver_type), solver_type(solver_type), saturated(saturated), log(log), task_ptr(task_ptr), ppc(ppc) {
        double infinity = lp_solver.get_infinity();
        int num_abstractions = abstractions.size();
        int num_operators = costs.size();
            // ppc is now a member variable

    saturated_costs_by_abstraction.reserve(num_abstractions);
    h_values_by_abstraction.reserve(num_abstractions);
    for (int i = 0; i < num_abstractions; ++i) {
        const Abstraction &abstraction = *abstractions[i];
        vector<int> h_values = abstraction.compute_goal_distances(costs);
        vector<int> saturated_costs =
            abstraction.compute_saturated_costs(h_values);
        h_values_by_abstraction.push_back(move(h_values));
        saturated_costs_by_abstraction.push_back(move(saturated_costs));
    }

    named_vector::NamedVector<lp::LPVariable> variables;
    variables.reserve(num_abstractions);
    for (int i = 0; i < num_abstractions; ++i) {
        // Objective coefficients are set below.
        variables.emplace_back(0, infinity, 0);
    }

    named_vector::NamedVector<lp::LPConstraint> constraints;
    constraints.reserve(num_operators);
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        lp::LPConstraint constraint(-infinity, costs[op_id]);
        for (int i = 0; i < num_abstractions; ++i) {
            if (saturated) {
                int scf_h = saturated_costs_by_abstraction[i][op_id];
                if (scf_h == -INF) {
                    // The constraint is always satisfied and we can ignore it.
                    continue;
                }
                if (scf_h != 0) {
                    constraint.insert(i, scf_h);
                }
            } else if (
                abstractions[i]->operator_is_active(op_id) &&
                costs[op_id] != 0) {
                constraint.insert(i, costs[op_id]);
            }
        }
        if (!constraint.empty()) {
            constraints.push_back(move(constraint));
        }
    }

    lp::LinearProgram lp(
        lp::LPObjectiveSense::MAXIMIZE, move(variables), move(constraints),
        lp_solver.get_infinity());
    lp_solver.load_problem(lp);
}

CostPartitioningHeuristic PhO::compute_cost_partitioning(
    const Abstractions &abstractions, const vector<int> &,
    const vector<int> &costs, const vector<int> &abstract_state_ids) {
    int num_abstractions = abstractions.size();
    cout << "Computing cost partitioning" << endl;
    int num_operators = costs.size();

    // Create a TaskProxy from the stored task_ptr
    TaskProxy task_proxy(*task_ptr);

    double min_h = std::numeric_limits<double>::infinity();
    for (int i = 0; i < num_abstractions; ++i) {
        int h = h_values_by_abstraction[i][abstract_state_ids[i]];
        if (h == INF) {
            // State is unsolvable.
            vector<int> zero_costs(num_operators, 0);
            CostPartitioningHeuristic cp_heuristic;
            for (int i = 0; i < num_abstractions; ++i) {
                vector<int> h_values =
                    abstractions[i]->compute_goal_distances(zero_costs);
                cp_heuristic.add_h_values(i, move(h_values));
            }
            return cp_heuristic;
        }
        lp_solver.set_objective_coefficient(i, h);
        if (h > 0 && h < min_h) {
            min_h = h;
        }
    }

    lp_solver.solve();
    assert(lp_solver.has_optimal_solution());
    vector<double> solution = lp_solver.extract_solution();
    if (log.is_at_least_debug()) {
        log << "Objective value: " << lp_solver.get_objective_value() << endl;
        log << "Solution: " << solution << endl;
    }

    CostPartitioningHeuristic cp_heuristic;
    for (int i = 0; i < num_abstractions; ++i) {
        double weight = solution[i];
        if (weight == 0.0) {
            // This abstraction is assigned a weight of zero, so we can skip it.
            continue;
        }
        vector<int> weighted_h_values;
        weighted_h_values.reserve(h_values_by_abstraction[i].size());
        for (int h : h_values_by_abstraction[i]) {
            assert(weight > 0.0);
            weighted_h_values.push_back(
                h == INF ? INF : static_cast<int>(weight * h));
        }
        cp_heuristic.add_h_values(i, move(weighted_h_values));
    }
    if (log.is_at_least_debug()) {
        log << "CP value: "
            << cp_heuristic.compute_heuristic(abstract_state_ids) << endl;
    }


    if (ppc) {
        // Print for every evaluated state (no guard)

        // Use a separate LP solver for the second LP (printing only)
        lp::LPSolver print_lp_solver(solver_type);

        // After solving the first LP
        double prev_obj_value = lp_solver.get_objective_value();

        // Prepare variables for the new LP
        named_vector::NamedVector<lp::LPVariable> variables;
        double infinity = lp_solver.get_infinity();
        // Compute M for the big-M constraint: if min_h > 0, use prev_obj_value / min_h, else use prev_obj_value.
        double M = (min_h > 0) ? (prev_obj_value / min_h) : prev_obj_value;

        // Add weight variables (as before)
        for (int i = 0; i < num_abstractions; ++i) {
            variables.emplace_back(0, infinity, 0); // objective coeff set below
        }

        // Add binary variables
        for (int i = 0; i < num_abstractions; ++i) {
            variables.emplace_back(0, 1, 0, true); // is_integer = true
        }

        // Prepare constraints (copy from first LP)
        named_vector::NamedVector<lp::LPConstraint> constraints;
        constraints.reserve(num_operators);
        for (int op_id = 0; op_id < num_operators; ++op_id) {
            lp::LPConstraint constraint(-infinity, costs[op_id]);
            for (int i = 0; i < num_abstractions; ++i) {
                if (saturated) {
                    int scf_h = saturated_costs_by_abstraction[i][op_id];
                    if (scf_h == -INF) {
                        // The constraint is always satisfied and we can ignore it.
                        continue;
                    }
                    if (scf_h != 0) {
                        constraint.insert(i, scf_h);
                    }
                } else if (
                    abstractions[i]->operator_is_active(op_id) &&
                    costs[op_id] != 0) {
                    constraint.insert(i, costs[op_id]);
                }
            }
            if (!constraint.empty()) {
                constraints.push_back(move(constraint));
            }
        }

        // Add constraint: sum_i w_i * h_i >= prev_obj_value
        lp::LPConstraint obj_constraint(prev_obj_value, infinity);
        for (int i = 0; i < num_abstractions; ++i) {
            obj_constraint.insert(i, h_values_by_abstraction[i][abstract_state_ids[i]]);
        }
        constraints.push_back(obj_constraint);

        // Add constraints: b_i * M >= w_i for each i
        for (int i = 0; i < num_abstractions; ++i) {
            lp::LPConstraint bin_constraint(0, infinity);
            bin_constraint.insert(i, -1); // -w_i
            bin_constraint.insert(num_abstractions + i, M); // +M * b_i
            constraints.push_back(bin_constraint);
        }

        // Set objective: minimize sum_i b_i * N_i
        for (int i = 0; i < num_abstractions; ++i) {
            int N_i = abstractions[i]->get_num_states(); // number of abstract states for abstraction i
            variables[num_abstractions + i].objective_coefficient = N_i;
        }

        // Build and solve the new LP
        lp::LinearProgram new_lp(
            lp::LPObjectiveSense::MINIMIZE, std::move(variables), std::move(constraints), infinity);
        print_lp_solver.load_problem(new_lp);
        print_lp_solver.solve();

        vector<double> min_solution = print_lp_solver.extract_solution();
        for (int i = 0; i < num_abstractions; ++i) {
            double b_i = min_solution[num_abstractions + i];
            if (b_i > 0.5) {
                const Projection *proj = dynamic_cast<const Projection *>(abstractions[i].get());
                if (proj) {
                    const vector<int> &pattern = proj->get_pattern();
                    cout << "Selected pattern for abstraction " << i << ": ";
                    for (int var : pattern) {
                        cout << var << " ";
                        cout << task_proxy.get_variables()[var].get_fact(0).get_name().erase(0, 5) << " ";
                    }
                    cout << endl;
                }
            }
        }
    }
    return cp_heuristic;
}

class PhoFeature
    : public plugins::TypedFeature<Evaluator, ScaledCostPartitioningHeuristic> {
public:
    PhoFeature() : TypedFeature("pho") {
        document_subcategory("heuristics_cost_partitioning");
        document_title("Post-hoc optimization heuristic");
        document_synopsis(
            "Compute the maximum over multiple PhO heuristics precomputed offline.");

    add_options_for_cost_partitioning_heuristic(*this, "pho");
    add_option<bool>("saturated", "saturate costs", "true");
    add_option<bool>("ppc", "enable post-processing constraint LP", "false");
    add_order_options(*this);
    lp::add_lp_solver_option_to_feature(*this);
    }

    virtual shared_ptr<ScaledCostPartitioningHeuristic> create_component(
        const plugins::Options &options) const override {
        shared_ptr<AbstractTask> scaled_costs_task = get_scaled_costs_task(
            options.get<shared_ptr<AbstractTask>>("transform"));

        TaskProxy task_proxy(*scaled_costs_task);
        vector<int> costs = task_properties::get_operator_costs(task_proxy);
        Abstractions abstractions = generate_abstractions(
            scaled_costs_task,
            options.get_list<shared_ptr<AbstractionGenerator>>("abstractions"));
        PhO pho(
            abstractions, costs, options.get<lp::LPSolverType>("lpsolver"),
            options.get<bool>("saturated"),
            utils::get_log_for_verbosity(
                options.get<utils::Verbosity>("verbosity")),
            scaled_costs_task,
            options.get<bool>("ppc"));
        CPFunction cp_function = [&pho](
                                     const Abstractions &abstractions_,
                                     const vector<int> &order_,
                                     const vector<int> &costs_,
                                     const vector<int> &abstract_state_ids) {
            return pho.compute_cost_partitioning(
                abstractions_, order_, costs_, abstract_state_ids);
        };
        vector<CostPartitioningHeuristic> cp_heuristics =
            get_cp_heuristic_collection_generator_from_options(options)
                ->generate_cost_partitionings(
                    task_proxy, abstractions, costs, cp_function);
        return plugins::make_shared_from_arg_tuples<
            ScaledCostPartitioningHeuristic>(
            move(abstractions), move(cp_heuristics),
            // TODO: extract dead ends.
            nullptr, scaled_costs_task, options.get<bool>("cache_estimates"),
            options.get<string>("description"),
            options.get<utils::Verbosity>("verbosity"));
    }
};

static plugins::FeaturePlugin<PhoFeature> _plugin;
}
