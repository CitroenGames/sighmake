#pragma once

#include "common/project_types.hpp"

namespace vcxproj {

// Reader for legacy Visual C++ project files (.vcproj, VS2003/2005/2008).
// Maps the old attribute-based XML format (numeric enums, <Tool> elements,
// <Files>/<Filter> tree) onto the same Project model the .vcxproj reader
// produces, so the BuildscriptWriter can consume either.
class VcprojReader {
public:
    VcprojReader() = default;

    // Parse a .vcproj file and return a Project
    Project read_vcproj(const std::string& filepath);
};

} // namespace vcxproj
