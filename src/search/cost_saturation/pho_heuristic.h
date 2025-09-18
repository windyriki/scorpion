#ifndef COST_SATURATION_PHO_HEURISTIC_H
#define COST_SATURATION_PHO_HEURISTIC_H

#include "types.h"

#include "../lp/lp_solver.h"
#include "../utils/logging.h"
#include "utils.h"

#include <vector>

namespace cost_saturation {
class PhO {
    lp::LPSolver lp_solver;
    lp::LPSolverType solver_type; // store solver type for second LP
    std::vector<std::vector<int>> h_values_by_abstraction;
    bool saturated;
    std::vector<std::vector<int>> saturated_costs_by_abstraction;
    utils::LogProxy log;
    std::shared_ptr<AbstractTask> task_ptr;
    bool ppc;

public:
    PhO(const Abstractions &abstractions, const std::vector<int> &costs,
        lp::LPSolverType solver_type, bool saturated,
        const utils::LogProxy &log,
        std::shared_ptr<AbstractTask> task_ptr,
        bool ppc);

    CostPartitioningHeuristic compute_cost_partitioning(
        const Abstractions &abstractions, const std::vector<int> &order,
        const std::vector<int> &costs,
        const std::vector<int> &abstract_state_ids);
};
}

#endif
