#ifndef COST_SATURATION_PHO_HEURISTIC_H
#define COST_SATURATION_PHO_HEURISTIC_H

#include "types.h"

#include "../lp/lp_solver.h"
#include "../utils/logging.h"
#include "utils.h"
#include "../task_proxy.h"

#include <vector>
#include <fstream>

namespace cost_saturation {
class PhO {
    lp::LPSolver lp_solver;
    lp::LPSolver print_lp_solver;
    std::vector<std::vector<int>> h_values_by_abstraction;
    bool saturated;
    std::vector<std::vector<int>> saturated_costs_by_abstraction;
    utils::LogProxy log;
    bool ppc;
    int num_solutions_lp;
    int num_unique_solutions_lp;
    TaskProxy task_proxy;
    std::string output_file;
    std::ofstream training_data_file;
    std::ofstream mapping_file;
    named_vector::NamedVector<lp::LPConstraint> build_lp_constraints(const Abstractions &abstractions, const std::vector<int> &costs) const;
    
    struct BaseLPComponents {
        named_vector::NamedVector<lp::LPVariable> variables;
        named_vector::NamedVector<lp::LPConstraint> constraints;
    };
    
    BaseLPComponents create_base_lp_components(
        const Abstractions &abstractions,
        const std::vector<int> &costs,
        const std::vector<int> &abstract_state_ids,
        double prev_obj_value,
        double min_ppc_obj_value,
        double M,
        double infinity,
        int num_abstractions) const;
    
    void compute_perfect_pattern_collection(
        const Abstractions &abstractions,
        const std::vector<int> &costs,
        const std::vector<int> &abstract_state_ids,
        double prev_obj_value,
        double min_h);

public:
    PhO(const Abstractions &abstractions, const std::vector<int> &costs,
        lp::LPSolverType solver_type, bool saturated,
        const utils::LogProxy &log,
        bool ppc,
        int num_solutions_lp,
        int num_unique_solutions_lp,
        std::shared_ptr<AbstractTask> task_ptr,
        std::string output_file);

    CostPartitioningHeuristic compute_cost_partitioning(
        const Abstractions &abstractions, const std::vector<int> &order,
        const std::vector<int> &costs,
        const std::vector<int> &abstract_state_ids);
};
}

#endif
