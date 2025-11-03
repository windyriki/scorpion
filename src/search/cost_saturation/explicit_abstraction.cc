#include "utils.h"
#include "explicit_abstraction.h"

#include "abstraction.h"
#include "types.h"

#include "../utils/collections.h"
#include "../utils/strings.h"
#include <memory>

using namespace std;

namespace cost_saturation {
static int convert_op_to_label(int op_id) {
    assert(op_id < 0);
    return -(op_id + 1);
}

static void dijkstra_search(
    const vector<vector<Successor>> &graph,
    const vector<int> &costs,
    priority_queues::AdaptiveQueue<int> &queue,
    const LabelIdToOps &label_id_to_ops,
    vector<int> &distances) {
    assert(all_of(costs.begin(), costs.end(), [](int c) {return c >= 0;}));
    vector<int> label_to_cost(label_id_to_ops.size(), INF);
    for (int idx = 0; idx < static_cast<int>(label_id_to_ops.size()); ++idx) {
        const auto &ops = label_id_to_ops[idx];
        for (int op_id : ops) {
            assert(utils::in_bounds(op_id, costs));
            assert(utils::in_bounds(idx, label_to_cost));
            label_to_cost[idx] = min(label_to_cost[idx], costs[op_id]);
        }
    }

    while (!queue.empty()) {
        pair<int, int> top_pair = queue.pop();
        int distance = top_pair.first;
        int state = top_pair.second;
        int state_distance = distances[state];
        assert(state_distance <= distance);
        if (state_distance < distance) {
            continue;
        }
        for (const Successor &transition : graph[state]) {
            int successor = transition.state;
            int op = transition.op;
            int op_cost;
            if (op >= 0) {
                assert(utils::in_bounds(op, costs));
                op_cost = costs[op];
            } else {
                int label_idx = convert_op_to_label(op);
                assert(utils::in_bounds(label_idx, label_to_cost));
                op_cost = label_to_cost[label_idx];
            }
            assert(op_cost >= 0);
            int successor_distance = (op_cost == INF) ? INF : state_distance + op_cost;
            assert(successor_distance >= 0);
            if (distances[successor] > successor_distance) {
                distances[successor] = successor_distance;
                queue.push(successor_distance, successor);
            }
        }
    }
}

ostream &operator<<(ostream &os, const Successor &successor) {
    os << "(" << successor.op << ", " << successor.state << ")";
    return os;
}

static vector<bool> get_active_operators_from_graph(
    const vector<vector<Successor>> &backward_graph, int num_ops, const LabelIdToOps &label_id_to_ops) {
    vector<bool> active_operators(num_ops, false);
    int num_states = backward_graph.size();
    for (int target = 0; target < num_states; ++target) {
        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            if (op_id >= 0) {
                assert(utils::in_bounds(op_id, active_operators));
                active_operators[op_id] = true;
            } else {
                int label_idx = convert_op_to_label(op_id);
                assert(utils::in_bounds(label_idx, label_id_to_ops));
                const auto &ops = label_id_to_ops[label_idx];
                for (int actual_op : ops) {
                    assert(utils::in_bounds(actual_op, active_operators));
                    active_operators[actual_op] = true;
                }
            }
        }
    }
    return active_operators;
}

ExplicitAbstraction::ExplicitAbstraction(
    unique_ptr<AbstractionFunction> abstraction_function,
    vector<vector<Successor>> &&backward_graph_,
    vector<bool> &&looping_operators,
    vector<int> &&goal_states,
    int min_ops_per_label,
    int min_occurrences_per_label)
    : Abstraction(move(abstraction_function)),
      num_non_label_transitions(0),
      num_label_transitions(0),
      num_labels(0),
      label_size_counts(),
      reused_label_size_counts(),
      ops_pool(),
      label_id_to_ops(),
      next_label_id(-1),
      backward_graph(label_reduction(backward_graph_, min_ops_per_label, min_occurrences_per_label)),
      active_operators(get_active_operators_from_graph(
                           backward_graph, looping_operators.size(), label_id_to_ops)),
      looping_operators(move(looping_operators)),
      goal_states(move(goal_states)) {
#ifndef NDEBUG
    for (int target = 0; target < get_num_states(); ++target) {
        // Check that no transition is stored multiple times.
        vector<Successor> copied_transitions = this->backward_graph[target];
        sort(copied_transitions.begin(), copied_transitions.end());
        assert(utils::is_sorted_unique(copied_transitions));
        // Check that we don't store self-loops.
        assert(all_of(
            copied_transitions.begin(), copied_transitions.end(),
            [target](const Successor &succ) { return succ.state != target; }));
    }
#endif
}

int ExplicitAbstraction::create_or_reuse_label(OpsToLabelId &ops_to_label_id, vector<int> &&ops) {
    assert(utils::is_sorted_unique(ops));
    ops_pool.push_back(move(ops));
    const auto &ops_slice = ops_pool.back();
    const auto [it, inserted] = ops_to_label_id.emplace(ops_slice, next_label_id);
    if (inserted) {
        label_id_to_ops.emplace_back(it->first);
        --next_label_id;

        ++num_labels;
        ++label_size_counts[ops_slice.size()];
    } else {
        ops_pool.pop_back();

        ++reused_label_size_counts[it->first.size()];
    }
    return it->second;
}

vector<vector<Successor>> ExplicitAbstraction::label_reduction(
    vector<vector<Successor>> &graph, int min_ops_per_label, int min_occurrences_per_label) {
    OpsToLabelId ops_to_label_id;

    int num_transitions_before_lr = 0;
    // Retrieve non-looping transitions.
    vector<vector<Successor>> new_graph(graph.size());

    if (min_ops_per_label == 0) {
        // Equivalence-based grouping: group by (op_id -> list of (src,target))
        phmap::flat_hash_map<vector<pair<int, int>>, vector<int>, PairVectorHash> equivalence_groups;
        phmap::flat_hash_map<int, vector<pair<int, int>>> op_to_transitions;

        // Collect transitions per op_id
        for (int target = 0; target < static_cast<int>(graph.size()); ++target) {
            for (const Successor &succ : graph[target]) {
                ++num_transitions_before_lr;
                op_to_transitions[succ.op].emplace_back(succ.state, target);
            }
        }

        // Group by unique list of transitions
        for (auto &[op, transitions] : op_to_transitions) {
            sort(transitions.begin(), transitions.end());
            equivalence_groups[transitions].push_back(op);
        }

        for (auto &[transitions, ops] : equivalence_groups) {
            if (ops.size() == 1 || static_cast<int>(transitions.size()) < min_occurrences_per_label) {
                for (int op : ops) {
                    for (const auto &[src, target] : transitions) {
                        ++num_non_label_transitions;
                        new_graph[target].emplace_back(op, src);
                    }
                }
            } else {
                sort(ops.begin(), ops.end());
                int label_id = create_or_reuse_label(ops_to_label_id, move(ops));

                for (const auto &[src, target] : transitions) {
                    ++num_label_transitions;
                    new_graph[target].emplace_back(label_id, src);
                }
            }
        }
    } else {
        // Map from (src, target) to list of operators
        auto transition_groups = phmap::flat_hash_map<pair<int, int>, vector<int>, SzudzikPairHash>{};
        for (int target = 0; target < static_cast<int>(graph.size()); ++target) {
            for (const Successor &succ : graph[target]) {
                ++num_transitions_before_lr;
                transition_groups[{succ.state, target}].push_back(succ.op);
            }
        }

        phmap::flat_hash_map<vector<int>, int, VectorHash> label_usage_counts;
        for (auto &[src_target, ops] : transition_groups) {
            if (static_cast<int>(ops.size()) >= min_ops_per_label) {
                sort(ops.begin(), ops.end());
                label_usage_counts[ops]++;
            }
        }

        for (auto &[src_target, ops] : transition_groups) {
            const auto &[src, target] = src_target;

            if (static_cast<int>(ops.size()) < min_ops_per_label ||
                label_usage_counts[ops] < min_occurrences_per_label) {
                for (int op : ops) {
                    ++num_non_label_transitions;
                    new_graph[target].emplace_back(op, src);
                }
            } else {
                int label_id = create_or_reuse_label(ops_to_label_id, move(ops));
                ++num_label_transitions;
                new_graph[target].emplace_back(label_id, src);
            }
        }
    }

    for (int target = 0; target < static_cast<int>(graph.size()); ++target) {
        new_graph[target].shrink_to_fit();
#ifndef NDEBUG
        utils::g_log << "Old Graph: " << target << graph[target] << endl;
        utils::g_log << "New Graph: " << target << new_graph[target] << endl;
#endif
    }

#ifndef NDEBUG
    for (int idx = 0; idx < static_cast<int>(label_id_to_ops.size()); ++idx) {
        const auto &ops = label_id_to_ops[idx];
        utils::g_log << "Label ID " << -(idx + 1) << ": [";
        for (int i = 0; i < static_cast<int>(ops.size()); ++i) {
            utils::g_log << ops[i];
            if (i < static_cast<int>(ops.size()) - 1)
                utils::g_log << ", ";
        }
        utils::g_log << "]" << endl;
    }
    utils::g_log << "Number of transitions (before label reduction): " << num_transitions_before_lr << endl;
    utils::g_log << "Number of transitions (after label reduction): " << num_non_label_transitions + num_label_transitions << endl;
    utils::g_log << "Change in transitions ((#non-label transitions+#label transitions)/#transitions): " <<
        static_cast<double>(num_non_label_transitions + num_label_transitions) / num_transitions_before_lr << endl;
    utils::g_log << "Number of non-label transitions: " << num_non_label_transitions << endl;
    utils::g_log << "Number of label transitions: " << num_label_transitions << endl;
    utils::g_log << "Number of labels: " << num_labels << endl;
    utils::g_log << "Label size counts: {";
    bool first = true;
    for (const auto & [size, count] : label_size_counts) {
        if (!first)
            utils::g_log << ", ";
        utils::g_log << "\"" << size << "\": " << count;
        first = false;
    }
    utils::g_log << "}" << endl;

    utils::g_log << "Number of reused labels: " << num_label_transitions - num_labels << endl;
    utils::g_log << "Reused label size counts: {";
    first = true;
    for (const auto & [size, count] : reused_label_size_counts) {
        if (!first)
            utils::g_log << ", ";
        utils::g_log << "\"" << size << "\": " << count;
        first = false;
    }
    utils::g_log << "}" << endl;
#endif

    return new_graph;
}

vector<int> ExplicitAbstraction::compute_goal_distances(const vector<int> &costs) const {
    vector<int> goal_distances(get_num_states(), INF);
    queue.clear();
    for (int goal_state : goal_states) {
        goal_distances[goal_state] = 0;
        queue.push(0, goal_state);
    }
    dijkstra_search(backward_graph, costs, queue, label_id_to_ops, goal_distances);
    return goal_distances;
}

vector<int> ExplicitAbstraction::compute_saturated_costs(
    const vector<int> &h_values) const {
    int num_operators = get_num_operators();
    vector<int> saturated_costs(num_operators, -INF);
    vector<int> saturated_label_costs(label_id_to_ops.size(), -INF);

    /* To prevent negative cost cycles we ensure that all operators
       inducing self-loops have non-negative costs. */
    for (int op_id = 0; op_id < num_operators; ++op_id) {
        if (looping_operators[op_id]) {
            saturated_costs[op_id] = 0;
        }
    }

    int num_states = backward_graph.size();
    for (int target = 0; target < num_states; ++target) {
        assert(utils::in_bounds(target, h_values));
        int target_h = h_values[target];
        if (target_h == INF) {
            continue;
        }

        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            int src = transition.state;
            assert(utils::in_bounds(src, h_values));
            int src_h = h_values[src];
            if (src_h == INF) {
                continue;
            }

            const int needed = src_h - target_h;
            if (op_id >= 0) {
                saturated_costs[op_id] = max(saturated_costs[op_id], needed);
            } else {
                int label_idx = convert_op_to_label(op_id);
                saturated_label_costs[label_idx] = max(saturated_label_costs[label_idx], needed);
            }
        }
    }

    for (int idx = 0; idx < static_cast<int>(label_id_to_ops.size()); ++idx) {
        const auto &ops = label_id_to_ops[idx];
        int label_cost = saturated_label_costs[idx];
        for (int op_id : ops) {
            saturated_costs[op_id] = max(saturated_costs[op_id], label_cost);
        }
    }
    return saturated_costs;
}

int ExplicitAbstraction::get_num_operators() const {
    return looping_operators.size();
}

int ExplicitAbstraction::get_num_states() const {
    return backward_graph.size();
}

bool ExplicitAbstraction::operator_is_active(int op_id) const {
    return active_operators[op_id];
}

bool ExplicitAbstraction::operator_induces_self_loop(int op_id) const {
    return looping_operators[op_id];
}

void ExplicitAbstraction::for_each_transition(
    const TransitionCallback &callback) const {
    int num_states = get_num_states();
    for (int target = 0; target < num_states; ++target) {
        for (const Successor &transition : backward_graph[target]) {
            int op_id = transition.op;
            int src = transition.state;
            if (op_id >= 0) {
                callback(Transition(src, op_id, target));
            } else {
                int label_idx = convert_op_to_label(op_id);
                assert(utils::in_bounds(label_idx, label_id_to_ops));
                const auto &ops = label_id_to_ops[label_idx];
                for (int actual_op : ops) {
                    callback(Transition(src, actual_op, target));
                }
            }
        }
    }
}

const vector<int> &ExplicitAbstraction::get_goal_states() const {
    return goal_states;
}

void ExplicitAbstraction::dump() const {
    int num_states = get_num_states();

    cout << "States: " << num_states << endl;
    cout << "Goal states: " << goal_states.size() << endl;
    cout << "Operators inducing state-changing transitions: "
         << count(active_operators.begin(), active_operators.end(), true)
         << endl;
    cout << "Operators inducing self-loops: "
         << count(looping_operators.begin(), looping_operators.end(), true)
         << endl;

    vector<bool> is_goal(num_states, false);
    for (int goal : goal_states) {
        is_goal[goal] = true;
    }

    cout << "digraph transition_system";
    cout << " {" << endl;
    for (int i = 0; i < num_states; ++i) {
        cout << "    node [shape = " << (is_goal[i] ? "doublecircle" : "circle")
             << "] " << i << ";" << endl;
    }
    for (int target = 0; target < num_states; ++target) {
        unordered_map<int, vector<int>> parallel_transitions;
        for (const Successor &succ : backward_graph[target]) {
            int src = succ.state;
            parallel_transitions[src].push_back(succ.op);
        }
        for (const auto &pair : parallel_transitions) {
            int src = pair.first;
            const vector<int> &operators = pair.second;
            cout << "    " << src << " -> " << target << " [label = \""
                 << utils::join(operators, "_") << "\"];" << endl;
        }
    }
    cout << "}" << endl;
}
}
