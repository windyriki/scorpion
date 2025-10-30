#include "command_line.h"
#include "git_revision.h"
#include "search_algorithm.h"

#include "task_utils/task_properties.h"
#include "tasks/root_task.h"
#include "utils/logging.h"
#include "utils/system.h"
#include "utils/timer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <unistd.h>
#include <sys/wait.h>

using namespace std;
using utils::ExitCode;

int main(int argc, const char **argv) {
    try {
        if (argc == 2 &&
            static_cast<string>(argv[1]) == "--internal-git-revision") {
            // We handle this option before registering event handlers to avoid
            // printing peak memory on exit.
            cout << g_git_revision << endl;
            exit(0);
        }
        utils::register_event_handlers();

        if (argc < 2) {
            utils::g_log << get_revision_info() << endl;
            utils::g_log << get_usage(argv[0]) << endl;
            utils::exit_with(ExitCode::SEARCH_INPUT_ERROR);
        }

        bool unit_cost = false;
        if (static_cast<string>(argv[1]) != "--help") {
            utils::g_log << get_revision_info() << endl;
            utils::g_log << "reading input..." << endl;
            tasks::read_root_task(cin);
            utils::g_log << "done reading input!" << endl;
            TaskProxy task_proxy(*tasks::g_root_task);
            unit_cost = task_properties::is_unit_cost(task_proxy);
        }

        // Check for automatic pattern size mode (max_pattern_size=-1) BEFORE parsing
        bool auto_pattern_size_mode = false;
        string search_arg_template;
        int search_arg_index = -1;
        
        for (int i = 1; i < argc; ++i) {
            if (static_cast<string>(argv[i]) == "--search" && i + 1 < argc) {
                search_arg_template = argv[i + 1];
                search_arg_index = i + 1;
                // Check if the search string contains max_pattern_size=-1
                if (search_arg_template.find("max_pattern_size=-1") != string::npos ||
                    search_arg_template.find("pattern_max_size=-1") != string::npos) {
                    auto_pattern_size_mode = true;
                }
            }
        }
        
        if (auto_pattern_size_mode) {
            
            utils::g_log << "Finding maximum feasible pattern size..." << endl;
            
            // Create pipe for reading child output
            int pipefd[2];
            if (pipe(pipefd) == -1) {
                cerr << "Pipe failed" << endl;
                utils::exit_with(ExitCode::SEARCH_CRITICAL_ERROR);
            }
            
            pid_t pid = fork();
            if (pid == 0) {
                // Child process - test with max_pattern_size=infinity
                close(pipefd[0]); // Close read end
                
                // Redirect stdout and stderr to pipe
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                
                string test_search = search_arg_template;
                
                // Replace max_pattern_size=-1 or pattern_max_size=-1 with infinity
                size_t pos = test_search.find("max_pattern_size=-1");
                if (pos != string::npos) {
                    test_search.replace(pos, 19, "max_pattern_size=infinity");
                }
                pos = test_search.find("pattern_max_size=-1");
                if (pos != string::npos) {
                    test_search.replace(pos, 19, "pattern_max_size=infinity");
                }
                
                // Disable ppc to skip CSV output
                size_t ppc_pos = test_search.find("ppc=true");
                if (ppc_pos != string::npos) {
                    test_search.replace(ppc_pos, 8, "ppc=false");
                }
                
                // Create test argv
                vector<string> argv_strings;
                vector<const char*> test_argv;
                
                for (int i = 0; i < argc; ++i) {
                    if (i == search_arg_index) {
                        argv_strings.push_back(test_search);
                        test_argv.push_back(argv_strings.back().c_str());
                    } else {
                        test_argv.push_back(argv[i]);
                    }
                }
                
                try {
                    shared_ptr<SearchAlgorithm> test_search_alg = 
                        parse_cmd_line(test_argv.size(), test_argv.data(), unit_cost);
                    // Exit immediately after plugin initialization (pattern generation is done)
                    _exit(0);
                } catch (...) {
                    _exit(1);
                }
            } else if (pid > 0) {
                // Parent process - read from pipe
                close(pipefd[1]); // Close write end
                                
                // Read output from pipe and find the maximum pattern size stored
                int max_feasible_size = 2; // Default fallback
                bool limit_reached = false;
                
                char buffer[4096];
                string output;
                string line_buffer;
                ssize_t count;
                
                while ((count = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                    buffer[count] = '\0';
                    output += buffer;
                    line_buffer += buffer;
                    
                    // Process complete lines to extract pattern size information
                    size_t newline_pos;
                    while ((newline_pos = line_buffer.find('\n')) != string::npos) {
                        string line = line_buffer.substr(0, newline_pos);
                        line_buffer = line_buffer.substr(newline_pos + 1);
                        
                        // Check for "Maximum generated ordered systematic pattern size: X"
                        // This line appears once at the end with the definitive maximum size
                        if (line.find("Maximum generated ordered systematic pattern size:") != string::npos) {
                            size_t size_pos = line.find("size:") + 5;
                            string size_str = line.substr(size_pos);
                            size_str.erase(0, size_str.find_first_not_of(" \t"));
                            size_str.erase(size_str.find_last_not_of(" \t\n\r") + 1);
                            try {
                                max_feasible_size = stoi(size_str);
                                limit_reached = true;
                                kill(pid, SIGTERM);
                                break;
                            } catch (...) {
                                // Ignore parse errors
                            }
                        }
                    }
                    
                    if (limit_reached) {
                        break;
                    }
                }
                close(pipefd[0]);
                
                // Wait for child to complete
                int status;
                waitpid(pid, &status, 0);
                
                utils::g_log << "Maximum feasible pattern size: " << max_feasible_size << endl;
                utils::g_log << "Running final search with max_pattern_size=" << max_feasible_size << endl;
                utils::g_log << "========================================\n" << endl;
                
                // Now run the actual search with the maximum feasible size
                string final_search = search_arg_template;
                
                // Replace max_pattern_size=-1 or pattern_max_size=-1 with the max_feasible_size
                size_t pos = final_search.find("max_pattern_size=-1");
                if (pos != string::npos) {
                    final_search.replace(pos, 19, "max_pattern_size=" + to_string(max_feasible_size));
                }
                pos = final_search.find("pattern_max_size=-1");
                if (pos != string::npos) {
                    final_search.replace(pos, 19, "pattern_max_size=" + to_string(max_feasible_size));
                }
                
                vector<string> argv_strings;
                vector<const char*> final_argv;
                
                for (int i = 0; i < argc; ++i) {
                    if (i == search_arg_index) {
                        argv_strings.push_back(final_search);
                        final_argv.push_back(argv_strings.back().c_str());
                    } else {
                        final_argv.push_back(argv[i]);
                    }
                }
                
                shared_ptr<SearchAlgorithm> search_algorithm = 
                    parse_cmd_line(final_argv.size(), final_argv.data(), unit_cost);
                
                if (search_algorithm) {
                    utils::Timer search_timer;
                    search_algorithm->search();
                    search_timer.stop();
                    utils::g_timer.stop();
                    
                    search_algorithm->save_plan_if_necessary();
                    search_algorithm->print_statistics();
                    utils::g_log << "Search time: " << search_timer << endl;
                    utils::g_log << "Total time: " << utils::g_timer << endl;
                    
                    ExitCode exitcode = ExitCode::SEARCH_UNSOLVED_INCOMPLETE;
                    if (search_algorithm->get_status() == SOLVED) {
                        exitcode = ExitCode::SUCCESS;
                    } else if (search_algorithm->get_status() == UNSOLVABLE) {
                        exitcode = ExitCode::SEARCH_UNSOLVABLE;
                    }
                    exit_with(exitcode);
                }
                
                utils::exit_with(ExitCode::SEARCH_INPUT_ERROR);
            } else {
                cerr << "Fork failed" << endl;
                utils::exit_with(ExitCode::SEARCH_CRITICAL_ERROR);
            }
        }

        // Normal mode - single search execution
        shared_ptr<SearchAlgorithm> search_algorithm =
            parse_cmd_line(argc, argv, unit_cost);

        utils::Timer search_timer;
        search_algorithm->search();
        search_timer.stop();
        utils::g_timer.stop();

        search_algorithm->save_plan_if_necessary();
        search_algorithm->print_statistics();
        utils::g_log << "Search time: " << search_timer << endl;
        utils::g_log << "Total time: " << utils::g_timer << endl;

        ExitCode exitcode = ExitCode::SEARCH_UNSOLVED_INCOMPLETE;
        if (search_algorithm->get_status() == SOLVED) {
            exitcode = ExitCode::SUCCESS;
        } else if (search_algorithm->get_status() == UNSOLVABLE) {
            exitcode = ExitCode::SEARCH_UNSOLVABLE;
        }
        exit_with(exitcode);
    } catch (const utils::ExitException &e) {
        /* To ensure that all destructors are called before the program exits,
           we raise an exception in utils::exit_with() and let main() return. */
        return static_cast<int>(e.get_exitcode());
    }
}
