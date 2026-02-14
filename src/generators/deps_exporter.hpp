#pragma once

#include "common/project_types.hpp"

namespace vcxproj {

// Exports the dependency graph of a Solution as a self-contained HTML file.
// Output file: <solution_name>_dependencies.html in the output directory.
// Returns true on success, false on failure.
bool export_dependencies_html(const Solution& solution, const std::string& output_dir);

} // namespace vcxproj
