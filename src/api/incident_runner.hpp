#pragma once

#include <string>
#include <vector>

namespace incident_runner {

// Run using argv-style inputs.
int run_incident_runner_main(int argc, char** argv);

// Convenience for tests / programmatic callers.
int run_incident_runner_cli(const std::vector<std::string>& args);

} // namespace incident_runner

