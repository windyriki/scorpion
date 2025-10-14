#include "pattern_collection_generator_manual.h"

#include "validation.h"

#include "../task_proxy.h"

#include "../plugins/plugin.h"
#include "../utils/logging.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace std;

namespace pdbs {
PatternCollectionGeneratorManual::PatternCollectionGeneratorManual(
    const vector<Pattern> &patterns, utils::Verbosity verbosity)
    : PatternCollectionGenerator(verbosity),
      patterns(make_shared<PatternCollection>(patterns)) {
}

string PatternCollectionGeneratorManual::name() const {
    return "manual pattern collection generator";
}

PatternCollectionInformation PatternCollectionGeneratorManual::compute_patterns(
    const shared_ptr<AbstractTask> &task) {
    if (log.is_at_least_normal()) {
        log << "Manual pattern collection: " << *patterns << endl;
    }
    TaskProxy task_proxy(*task);
    return PatternCollectionInformation(task_proxy, patterns, log);
}

class PatternCollectionGeneratorManualFeature
    : public plugins::TypedFeature<
          PatternCollectionGenerator, PatternCollectionGeneratorManual> {
public:
    PatternCollectionGeneratorManualFeature()
        : TypedFeature("manual_patterns") {
        add_list_option<Pattern>(
            "patterns",
            "list of patterns (which are lists of variable numbers of the planning "
            "task).");
        add_generator_options_to_feature(*this);
    }

    virtual shared_ptr<PatternCollectionGeneratorManual> create_component(
        const plugins::Options &opts) const override {
        return plugins::make_shared_from_arg_tuples<
            PatternCollectionGeneratorManual>(
            opts.get_list<Pattern>("patterns"),
            get_generator_arguments_from_options(opts));
    }
};

static plugins::FeaturePlugin<PatternCollectionGeneratorManualFeature> _plugin;

// Helper function to parse pattern collections from a file
// Expected format: ["var1", "var2"], ["var3", "var4", "var5"]
static vector<vector<string>> parse_pattern_file(const string &filename) {
    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Error: Could not open pattern file: " << filename << endl;
        utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
    }

    vector<vector<string>> patterns;
    string line;
    while (getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse patterns separated by commas at the top level
        // Format: ["var1", "var2"], ["var3", "var4"]
        size_t pos = 0;
        while (pos < line.size()) {
            // Find start of pattern '['
            size_t start = line.find('[', pos);
            if (start == string::npos) break;
            
            // Find end of pattern ']'
            size_t end = line.find(']', start);
            if (end == string::npos) {
                cerr << "Error: Malformed pattern in file (missing ']'): " << line << endl;
                utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
            }

            // Extract pattern content between brackets
            string pattern_str = line.substr(start + 1, end - start - 1);
            vector<string> pattern;

            // Parse comma-separated predicates, but don't split on commas inside parentheses
            // e.g., ["at(ball1, rooma)", "free(left)"] should split into 2 items, not 3
            size_t pred_start = 0;
            int paren_depth = 0;
            bool in_quotes = false;
            
            for (size_t i = 0; i <= pattern_str.size(); ++i) {
                char ch = (i < pattern_str.size()) ? pattern_str[i] : ','; // treat end as comma
                
                if (ch == '"' && (i == 0 || pattern_str[i-1] != '\\')) {
                    in_quotes = !in_quotes;
                } else if (!in_quotes) {
                    if (ch == '(') {
                        paren_depth++;
                    } else if (ch == ')') {
                        paren_depth--;
                    } else if (ch == ',' && paren_depth == 0) {
                        // Found a top-level comma - extract token
                        string token = pattern_str.substr(pred_start, i - pred_start);
                        // Trim whitespace
                        token.erase(0, token.find_first_not_of(" \t\n\r"));
                        token.erase(token.find_last_not_of(" \t\n\r") + 1);
                        // Remove quotes if present
                        if (!token.empty() && token.front() == '"') {
                            token = token.substr(1);
                        }
                        if (!token.empty() && token.back() == '"') {
                            token.pop_back();
                        }
                        if (!token.empty()) {
                            pattern.push_back(token);
                        }
                        pred_start = i + 1;
                    }
                }
            }

            if (!pattern.empty()) {
                patterns.push_back(pattern);
            }
            pos = end + 1;
        }
    }

    if (patterns.empty()) {
        cerr << "Warning: No patterns found in file: " << filename << endl;
    }

    return patterns;
}

class PatternCollectionGeneratorManualString : public PatternCollectionGenerator {
    vector<vector<string>> string_patterns;
public:
    PatternCollectionGeneratorManualString(
        const vector<vector<string>> &patterns,
        utils::Verbosity verbosity)
        : PatternCollectionGenerator(verbosity),
          string_patterns(patterns) {
    }

    virtual string name() const override {
        return "manual pattern collection generator (string-based)";
    }

    virtual PatternCollectionInformation compute_patterns(
        const shared_ptr<AbstractTask> &task) override {
        // Convert string patterns to integer patterns by looking up variable names
        // Variable names are grounded predicates like "holding(a)", "on(a, b)", etc.
        TaskProxy task_proxy(*task);
        auto pattern_collection = make_shared<PatternCollection>();
        
        for (const auto &string_pattern : string_patterns) {
            Pattern int_pattern;
            for (const string &var_name : string_pattern) {
                // Search for variable by matching the full grounded predicate name
                // The task stores these as "Atom <predicate>(<args>)" where we match <predicate>(<args>)
                int var_id = -1;
                for (size_t i = 0; i < task_proxy.get_variables().size(); ++i) {
                    string fact_name = task_proxy.get_variables()[i].get_fact(0).get_name();
                    // Remove "Atom " prefix (5 chars) to get the grounded predicate
                    if (fact_name.size() >= 5) {
                        fact_name = fact_name.substr(5);
                    }
                    // Match the full grounded predicate (e.g., "holding(a)" or "on(a, b)")
                    if (fact_name == var_name) {
                        var_id = static_cast<int>(i);
                        break;
                    }
                }
                if (var_id == -1) {
                    cerr << "Error: Grounded predicate '" << var_name << "' not found in task." << endl;
                    cerr << "Make sure the predicate name and arguments match exactly (including parentheses and spacing)." << endl;
                    cerr << "Available predicates:" << endl;
                    for (size_t i = 0; i < task_proxy.get_variables().size(); ++i) {
                        string fact_name = task_proxy.get_variables()[i].get_fact(0).get_name();
                        if (fact_name.size() >= 5) {
                            fact_name = fact_name.substr(5);
                        }
                        cerr << "  " << i << ": " << fact_name << endl;
                    }
                    utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
                }
                int_pattern.push_back(var_id);
            }
            pattern_collection->push_back(int_pattern);
        }
        
        if (log.is_at_least_normal()) {
            log << "Manual pattern collection (from strings): " << *pattern_collection << endl;
        }
        return PatternCollectionInformation(task_proxy, pattern_collection, log);
    }
};

class PatternCollectionGeneratorManualStringFeature
    : public plugins::TypedFeature<
          PatternCollectionGenerator, PatternCollectionGeneratorManualString> {
public:
    PatternCollectionGeneratorManualStringFeature()
        : TypedFeature("manual_patterns_string") {
        add_list_option<vector<string>>(
            "patterns",
            "list of patterns where each pattern is a list of grounded predicates "
            "(as strings). Format: 'predicate(arg1, arg2, ...)'. Must match task facts exactly.",
            "[]");
        add_option<string>(
            "file",
            "path to a file containing pattern collection. "
            "Format: [\"holding(a)\", \"clear(b)\"], [\"on(a, b)\", \"handempty()\"]",
            "");
        add_generator_options_to_feature(*this);
    }

    virtual shared_ptr<PatternCollectionGeneratorManualString> create_component(
        const plugins::Options &opts) const override {
        vector<vector<string>> string_patterns = opts.get_list<vector<string>>("patterns");
        string file_path = opts.get<string>("file");

        // If file is specified, read patterns from file (overrides inline patterns)
        if (!file_path.empty()) {
            string_patterns = parse_pattern_file(file_path);
        }

        if (string_patterns.empty()) {
            cerr << "Error: No patterns specified. Provide either 'patterns' or 'file' option." << endl;
            utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
        }

        return plugins::make_shared_from_arg_tuples<
            PatternCollectionGeneratorManualString>(
            string_patterns,
            get_generator_arguments_from_options(opts));
    }
};

static plugins::FeaturePlugin<PatternCollectionGeneratorManualStringFeature> _plugin_string;
}
