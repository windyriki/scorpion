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
#include "../ext/parallel_hashmap/phmap.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <limits>

using namespace std;

namespace std {
// Custom hash for vector<int> needed for phmap::flat_hash_set<vector<int>>
template <>
struct hash<std::vector<int>> {
    size_t operator()(const std::vector<int>& v) const noexcept {
        size_t h = 0;
        for (int x : v) {
            h = phmap::HashState().combine(h, x);
        }
        return h;
    }
};
}

namespace cost_saturation {
    constexpr size_t ATOM_PREFIX_LEN = 5;

    PhO::BaseLPComponents PhO::create_base_lp_components(
        const Abstractions &abstractions,
        const vector<int> &costs,
        const vector<int> &abstract_state_ids,
        double prev_obj_value,
        double min_ppc_obj_value,
        double M,
        double infinity,
        int num_abstractions) const {
        
        BaseLPComponents components;
        
        // Create variables
        components.variables.reserve(2 * num_abstractions);
        for (int i = 0; i < num_abstractions; ++i) {
            components.variables.emplace_back(0, infinity, 0); 
        }
        for (int i = 0; i < num_abstractions; ++i) {
            components.variables.emplace_back(0, 1, 0, true);
        }
        
        // Build base constraints
        components.constraints = this->build_lp_constraints(abstractions, costs);
        
        // Add constraint: sum_i w_i * h_i == prev_obj_value
        lp::LPConstraint constraint_w_eq(prev_obj_value, prev_obj_value);
        for (int i = 0; i < num_abstractions; ++i) {
            constraint_w_eq.insert(i, h_values_by_abstraction[i][abstract_state_ids[i]]);
        }
        components.constraints.push_back(constraint_w_eq);
        
        // Add constraints: b_i * M >= w_i for each i
        for (int i = 0; i < num_abstractions; ++i) {
            lp::LPConstraint constraint_b(0, infinity);
            constraint_b.insert(i, -1);
            constraint_b.insert(num_abstractions + i, M);
            components.constraints.push_back(constraint_b);
        }
        
        // Add constraint: sum_i N_i * b_i == min_ppc_obj_value (fix to optimal objective)
        lp::LPConstraint constraint_optimal_size(min_ppc_obj_value, min_ppc_obj_value);
        for (int i = 0; i < num_abstractions; ++i) {
            int N_i = abstractions[i]->get_num_states();
            constraint_optimal_size.insert(num_abstractions + i, N_i);
        }
        components.constraints.push_back(constraint_optimal_size);
        
        return components;
    }

    named_vector::NamedVector<lp::LPConstraint> PhO::build_lp_constraints(
    const Abstractions &abstractions,
    const vector<int> &costs) const {
        int num_abstractions = abstractions.size();
        int num_operators = costs.size();

        named_vector::NamedVector<lp::LPConstraint> constraints;
        constraints.reserve(num_operators);
        for (int op_id = 0; op_id < num_operators; ++op_id) {
            lp::LPConstraint constraint(-lp_solver.get_infinity(), costs[op_id]);
            for (int i = 0; i < num_abstractions; ++i) {
                if (saturated) { 
                    int scf_h = saturated_costs_by_abstraction[i][op_id]; 
                    if (scf_h == -INF) continue;
                    if (scf_h != 0) constraint.insert(i, scf_h);
                } else if (abstractions[i]->operator_is_active(op_id) && costs[op_id] != 0) {
                    constraint.insert(i, costs[op_id]);
                }
            }
            if (!constraint.empty()) {
                constraints.push_back(move(constraint));
            }
        }
        return constraints;
    }

    void PhO::compute_perfect_pattern_collection(
        const Abstractions &abstractions,
        const vector<int> &costs,
        const vector<int> &abstract_state_ids,
        double prev_obj_value,
        double min_h) {
        
            int num_abstractions = abstractions.size();
            
            // After solving the first LP
            double M = (min_h > 0) ? (prev_obj_value / min_h) : prev_obj_value;
            double infinity = print_lp_solver.get_infinity();
    
            named_vector::NamedVector<lp::LPVariable> variables;
            variables.reserve(2* num_abstractions);
            for (int i = 0; i < num_abstractions; ++i) {
                variables.emplace_back(0, infinity, 0); 
            }
            for (int i = 0; i < num_abstractions; ++i) {
                variables.emplace_back(0, 1, 0, true);
            }
    
            named_vector::NamedVector<lp::LPConstraint> constraints =
                this->build_lp_constraints(abstractions, costs);
    
            // Add constraint: sum_i w_i * h_i == prev_obj_value (The optimal heuristic value from the first LP)
            lp::LPConstraint constraint_w_eq(prev_obj_value, prev_obj_value);
            for (int i = 0; i < num_abstractions; ++i) {
                constraint_w_eq.insert(i, h_values_by_abstraction[i][abstract_state_ids[i]]);
            }
            constraints.push_back(constraint_w_eq);
    
            // Add constraints: b_i * M >= w_i for each i
            for (int i = 0; i < num_abstractions; ++i) {
                lp::LPConstraint constraint_b(0, infinity);
                constraint_b.insert(i, -1);
                constraint_b.insert(num_abstractions + i, M);
                constraints.push_back(constraint_b);
            }
    
            // Set objective: Minimize sum_i N_i * b_i
            for (int i = 0; i < num_abstractions; ++i) {
                int N_i = abstractions[i]->get_num_states();
                variables[num_abstractions + i].objective_coefficient = N_i;
            }
    
            // 1. Solve once to find the minimum objective value (min PPC size)
            lp::LinearProgram new_lp(
                lp::LPObjectiveSense::MINIMIZE, std::move(variables), std::move(constraints), infinity);
            print_lp_solver.load_problem(new_lp);
            
            print_lp_solver.solve();
            if (!print_lp_solver.has_optimal_solution()) {
                cout << "No solution found in the second LP." << endl;
                return;
            }
            double min_ppc_obj_value = print_lp_solver.get_objective_value();
            cout << "Minimum PPC objective value: " << min_ppc_obj_value << endl;
            
            // 2. Rebuild the LP with an equality constraint to fix the objective value.
            named_vector::NamedVector<lp::LPVariable> variables2;
            variables2.reserve(2* num_abstractions);
            for (int i = 0; i < num_abstractions; ++i) {
                variables2.emplace_back(0, infinity, 0); 
            }
            for (int i = 0; i < num_abstractions; ++i) {
                variables2.emplace_back(0, 1, 0, true);
            }
            
            named_vector::NamedVector<lp::LPConstraint> constraints2 =
                this->build_lp_constraints(abstractions, costs);
            
            // Add constraint: sum_i w_i * h_i == prev_obj_value
            lp::LPConstraint constraint_w_eq2(prev_obj_value, prev_obj_value);
            for (int i = 0; i < num_abstractions; ++i) {
                constraint_w_eq2.insert(i, h_values_by_abstraction[i][abstract_state_ids[i]]);
            }
            constraints2.push_back(constraint_w_eq2);
            
            // Add constraints: b_i * M >= w_i for each i
            for (int i = 0; i < num_abstractions; ++i) {
                lp::LPConstraint constraint_b2(0, infinity);
                constraint_b2.insert(i, -1);
                constraint_b2.insert(num_abstractions + i, M);
                constraints2.push_back(constraint_b2);
            }
            
            // Add constraint: sum_i N_i * b_i == min_ppc_obj_value (fix to optimal objective)
            lp::LPConstraint constraint_optimal_size(min_ppc_obj_value, min_ppc_obj_value);
            for (int i = 0; i < num_abstractions; ++i) {
                int N_i = abstractions[i]->get_num_states();
                constraint_optimal_size.insert(num_abstractions + i, N_i);
            }
            constraints2.push_back(constraint_optimal_size);
            
            // Reload the LP with the new constraint
            lp::LinearProgram new_lp2(
                lp::LPObjectiveSense::MINIMIZE, std::move(variables2), std::move(constraints2), infinity);
            print_lp_solver.load_problem(new_lp2);
                    
            int num_solutions = (num_solutions_lp == std::numeric_limits<int>::max()) 
                            ? num_abstractions 
                            : std::min(num_solutions_lp, num_abstractions);
            
            size_t max_pattern_size = 0;
            int max_num_patterns = 0;
            phmap::flat_hash_set<vector<int>> seen_collections;
            
            // Pre-compute state and goal atoms (before the loop)
            vector<string> state_atoms;
            State state = task_proxy.get_initial_state();
            state_atoms.reserve(state.size());
            for (size_t i = 0; i < state.size(); ++i) {
                string state_atom = state[i].get_name();
                if (state_atom.front() != 'N') {
                    state_atoms.push_back(state_atom.substr(ATOM_PREFIX_LEN));
                }
            }
            sort(state_atoms.begin(), state_atoms.end());
    
            vector<string> goal_atoms;
            goal_atoms.reserve(task_proxy.get_goals().size());
            for (size_t i = 0; i < task_proxy.get_goals().size(); ++i) {
                string goal_name = task_proxy.get_goals()[i].get_name();
                goal_atoms.push_back(goal_name.substr(ATOM_PREFIX_LEN));
            }
            sort(goal_atoms.begin(), goal_atoms.end());
            
            // Store all no-good cuts as permanent constraints by rebuilding LP each iteration
            vector<lp::LPConstraint> all_nogood_cuts;
            
            // Create base LP components once before the loop
            BaseLPComponents base_components = create_base_lp_components(
                abstractions, costs, abstract_state_ids, prev_obj_value,
                min_ppc_obj_value, M, infinity, num_abstractions);
            
            #ifndef NDEBUG
            vector<double> last_valid_solution;
            #endif
    
            // The iterative loop now finds ALL optimal solutions
            for (int sol = 0; sol < num_solutions; ++sol) {
                
                // Copy base components and add accumulated no-good cuts
                named_vector::NamedVector<lp::LPVariable> variables_iter = base_components.variables;
                named_vector::NamedVector<lp::LPConstraint> constraints_iter = base_components.constraints;
                
                // Add ALL accumulated no-good cuts as permanent constraints
                for (const auto& cut : all_nogood_cuts) {
                    constraints_iter.push_back(cut);
                }
                
                // Reload the LP with all constraints including no-good cuts
                lp::LinearProgram lp_iter(
                    lp::LPObjectiveSense::MINIMIZE, std::move(variables_iter), 
                    std::move(constraints_iter), infinity);
                print_lp_solver.load_problem(lp_iter);
                
                print_lp_solver.solve(); 
                
                if (!print_lp_solver.has_optimal_solution()) {
                    // No more solutions exist (either optimal or feasible)
                    cout << "No more optimal solutions found after " << sol << " iterations." << endl;
                    break;
                }
    
                vector<double> min_solution = print_lp_solver.extract_solution();
    
                #ifndef NDEBUG
                last_valid_solution = min_solution;
                #endif
                
                // Build sorted vector of selected abstraction indices
                vector<int> selected_indices;
                for (int i = 0; i < num_abstractions; ++i) {
                    if (min_solution[num_abstractions + i] > 0.5) {
                        selected_indices.push_back(i);
                    }
                }
                sort(selected_indices.begin(), selected_indices.end());
                
                // Check uniqueness
                auto [iter, inserted] = seen_collections.insert(selected_indices);
                
                if (inserted) {
                    // Found a unique optimal solution. Process it.
                    vector<vector<string>> pattern_collections;
                    pattern_collections.reserve(selected_indices.size());
                    for (int idx : selected_indices) {
                        const Projection *proj = dynamic_cast<const Projection *>(abstractions[idx].get());
                        if (proj) {
                            const vector<int> &pattern = proj->get_pattern();
                            vector<string> pattern_strings;
                            pattern_strings.reserve(pattern.size());
                            for (int var : pattern) {
                                string var_name = task_proxy.get_variables()[var].get_fact(0).get_name();
                                pattern_strings.push_back(var_name.substr(ATOM_PREFIX_LEN));
                            }
                            sort(pattern_strings.begin(), pattern_strings.end());
                            pattern_collections.push_back(move(pattern_strings));
                        }
                    }
                    sort(pattern_collections.begin(), pattern_collections.end());
    
                    // Update stats
                    int num_patterns = pattern_collections.size();
                    if (num_patterns > max_num_patterns) max_num_patterns = num_patterns;
                    for (const auto &pattern_strings : pattern_collections) {
                        if (pattern_strings.size() > max_pattern_size) {
                            max_pattern_size = pattern_strings.size();
                        }
                    }
    
                    // Write to file
                    bool first = true;
                    for (const auto &atom : state_atoms) {
                        if (!first) training_data_file << ",";
                        first = false;
                        training_data_file << atom;
                    }
                    training_data_file << ";";
    
                    first = true;
                    for (const auto &atom : goal_atoms) {
                        if (!first) training_data_file << ",";
                        first = false;
                        training_data_file << atom;
                    }
                    training_data_file << ";[";
    
                    for (size_t i = 0; i < pattern_collections.size(); ++i) {
                        if (i > 0) training_data_file << ",";
                        training_data_file << "[";
                        for (size_t j = 0; j < pattern_collections[i].size(); ++j) {
                            if (j > 0) training_data_file << ",";
                            training_data_file << pattern_collections[i][j];
                        }
                        training_data_file << "]";
                    }
                    training_data_file << "]" << endl;
    
                    if (num_unique_solutions_lp != std::numeric_limits<int>::max() &&
                        (int)seen_collections.size() >= num_unique_solutions_lp) {
                        cout << "Found " << seen_collections.size() 
                            << " unique solutions, stopping LP iterations." << endl;
                        break;
                    }
                }
                
                // Create and store no-good cut to exclude this solution (whether unique or duplicate)
                // -sum_{i in selected} b_i + sum_{i not in selected} b_i >= 1 - |selected|
                int num_selected = selected_indices.size();
                lp::LPConstraint nogood_cut(1 - num_selected, print_lp_solver.get_infinity());
                for (int i = 0; i < num_abstractions; ++i) {
                    if (std::binary_search(selected_indices.begin(), selected_indices.end(), i)) {
                        nogood_cut.insert(num_abstractions + i, -1); // -b_i
                    } else {
                        nogood_cut.insert(num_abstractions + i, 1);  // b_i
                    }
                }            
                all_nogood_cuts.push_back(move(nogood_cut));
            }
            
            // Summary output
            cout << "Maximum used pattern size: " << max_pattern_size << endl;
            cout << "Maximum number of patterns in PPC: " << max_num_patterns << endl;
            cout << "Total unique pattern collections found: " << seen_collections.size() << endl;
            
            
            #ifndef NDEBUG
                bool first_fact_debug = true;
                // Print out facts of the current state
                cout << "Initial state:" << endl;
                for (size_t i = 0; i < state.size(); ++i) {
                    string state_atom = state[i].get_name();
                    // Ignore negatedAtoms as they can be implicitly assumed to be false if not present
                    if (state_atom.front() == 'N') {
                        continue;
                    }
                    if(!first_fact_debug) cout << ", ";
                    cout << i << " ";
                    // Remove "Atom" prefix
                    string state_atom_name = task_proxy.get_variables()[i].get_fact(0).get_name();
                    cout << state_atom_name.substr(ATOM_PREFIX_LEN);
                    first_fact_debug = false;
                }
                cout << endl;
                cout << "Perfect pattern collection:" << endl;
                
                // Collect all selected patterns with their string representations for sorting
                vector<vector<string>> debug_patterns;
                for (int i = 0; i < num_abstractions; ++i) {
                    double b_i = last_valid_solution[num_abstractions + i];
                    if (b_i > 0.5) {
                        const Projection *proj = dynamic_cast<const Projection *>(abstractions[i].get());
                        if (proj) {
                            const vector<int> &pattern = proj->get_pattern();
                            vector<string> pattern_strings;
                            pattern_strings.reserve(pattern.size());
                            for (int var : pattern) {
                                string pattern_name = task_proxy.get_variables()[var].get_fact(0).get_name();
                                pattern_strings.push_back(to_string(var) + " " + pattern_name.substr(ATOM_PREFIX_LEN));
                            }
    
                            // Sort each pattern by name (ignoring numeric prefix)
                            sort(pattern_strings.begin(), pattern_strings.end(),
                                [](const string &a, const string &b) {
                                    string name_a = a.substr(a.find(' ') + 1);
                                    string name_b = b.substr(b.find(' ') + 1);
                                    return name_a < name_b;
                                });
    
                            debug_patterns.push_back(move(pattern_strings));
                        }
                    }
                }
    
                // Sort the outer pattern collection lexicographically by their string contents
                sort(debug_patterns.begin(), debug_patterns.end(),
                    [](const vector<string> &a, const vector<string> &b) {
                        return a < b;
                    });
    
                // Print sorted patterns
                for (const auto &pattern : debug_patterns) {
                    cout << "[";
                    for (size_t j = 0; j < pattern.size(); ++j) {
                        cout << pattern[j];
                        if (j != pattern.size() - 1)
                            cout << ", ";
                    }
                    cout << "]" << endl;
                }
    
            #endif
    }

    /*
    The implementation currently computes weighted lookup tables for PhO and holds them in memory. A
    more efficient implementation would only store the weights and compute the weighted heuristic
    values on the fly when evaluating a state.
  */
  PhO::PhO(
      const Abstractions &abstractions, const vector<int> &costs,
      lp::LPSolverType solver_type, bool saturated, const utils::LogProxy &log, 
      bool ppc, int num_solutions_lp, int num_unique_solutions_lp, std::shared_ptr<AbstractTask> task_ptr, string output_file)
      : lp_solver(solver_type),
      print_lp_solver(solver_type),
      saturated(saturated), log(log),
      ppc(ppc),
      num_solutions_lp(num_solutions_lp),
      num_unique_solutions_lp(num_unique_solutions_lp),
      task_proxy(*task_ptr),
      output_file(output_file) {
          double infinity = lp_solver.get_infinity();
          int num_abstractions = abstractions.size();
          
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
            
            named_vector::NamedVector<lp::LPConstraint> constraints = 
                build_lp_constraints(abstractions, costs);
                
            lp::LinearProgram lp(
                lp::LPObjectiveSense::MAXIMIZE, move(variables), move(constraints),
                lp_solver.get_infinity());
            lp_solver.load_problem(lp);
        
        // Print out header for training data file
        string filename = output_file + "_ppc_training_data.csv";
        ofstream clear_file(filename, ios::out | ios::trunc);
        clear_file.close();
        training_data_file.open(filename, ios::app);
        if (!training_data_file.is_open()) {
            cerr << "Failed to open training data file: " << filename << endl;
        } else {
            training_data_file << "state_atoms;goal_atoms;perfect_pattern_collection" << endl;
        }

        // Print out mapping
        string mapping_filename = output_file + "_mapping.csv";
        clear_file.open(mapping_filename, ios::out | ios::trunc);
        clear_file.close();
        mapping_file.open(mapping_filename, ios::app);
        if (!mapping_file.is_open()) {
            cerr << "Failed to open mapping file: " << mapping_filename << endl;
        } else {
            mapping_file << "id;string" << endl;
        }
        for (size_t i = 0; i < task_proxy.get_variables().size(); ++i) { 
            mapping_file << i << ";";  
            string atom_name = task_proxy.get_variables()[i].get_fact(0).get_name();         
            mapping_file << atom_name.substr(ATOM_PREFIX_LEN) << endl;
        }
}

CostPartitioningHeuristic PhO::compute_cost_partitioning(
    const Abstractions &abstractions, const vector<int> &,
    const vector<int> &costs, const vector<int> &abstract_state_ids) {
    int num_abstractions = abstractions.size();
    int num_operators = costs.size();

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
        if (h < min_h) {
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
        compute_perfect_pattern_collection(
            abstractions, costs, abstract_state_ids, 
            lp_solver.get_objective_value(), min_h);
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
    add_option<bool>("ppc", "enable second LP to compute a perfect pattern collection for the state", "false");
    add_option<int>("num_solutions_lp", "number of LP solutions to generate by iteratively excluding abstractions; use infinity for all solutions", "infinity", plugins::Bounds("1", "infinity"));
    add_option<int>("num_unique_solutions_lp", "stop after finding this many unique pattern collections; use infinity to find all unique solutions", "infinity", plugins::Bounds("1", "infinity"));
    add_option<string>("output_file", "file to output the perfect pattern collection + mapping",  "\"test\"");
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
            options.get<bool>("ppc"),
            options.get<int>("num_solutions_lp"),
            options.get<int>("num_unique_solutions_lp"),
            scaled_costs_task,
            options.get<string>("output_file"));
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