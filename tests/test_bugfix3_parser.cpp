#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;

namespace {

Solution bf3_parse(const std::string& text) {
    BuildscriptParser parser;
    return parser.parse_string(text, "/tmp/bf3");
}

}  // namespace

TEST_CASE("An explicit rtti = false in a config section beats a project-level true", "[bugfix3_parser]") {
    const Solution sol = bf3_parse(R"(
[solution]
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
rtti = true

[config:Debug|x64]
rtti = false
)");
    const auto& cfgs = sol.projects[0].configurations;
    CHECK_FALSE(cfgs.at("Debug|x64").cl_compile.runtime_type_info);
    CHECK(cfgs.at("Release|x64").cl_compile.runtime_type_info);
}

TEST_CASE("Explicit false bool settings in config sections beat project-level and template trues", "[bugfix3_parser]") {
    const Solution sol = bf3_parse(R"(
[solution]
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
mp = true
fixed_base_address = true

[config:Debug|x64]
mp = false
fixed_base_address = false

[config:Release|x64]
string_pooling = true
generate_map_file = true
treat_warning_as_error = true
suppress_startup_banner = true

[config:Custom|x64] : Template:Release
string_pooling = false
generate_map_file = false
treat_warning_as_error = false
suppress_startup_banner = false
)");
    const auto& cfgs = sol.projects[0].configurations;
    const auto& dbg = cfgs.at("Debug|x64");
    CHECK_FALSE(dbg.cl_compile.multi_processor_compilation);
    CHECK_FALSE(dbg.link.fixed_base_address);
    const auto& custom = cfgs.at("Custom|x64");
    CHECK_FALSE(custom.cl_compile.string_pooling);
    CHECK_FALSE(custom.link.generate_map_file);
    CHECK_FALSE(custom.cl_compile.treat_warning_as_error);
    CHECK_FALSE(custom.link.suppress_startup_banner);
    CHECK(cfgs.at("Release|x64").cl_compile.string_pooling);
}

TEST_CASE("A template's explicit false wins over a derived configuration's default true", "[bugfix3_parser]") {
    const Solution sol = bf3_parse(R"(
[solution]
configurations = Release, Custom
platforms = x64

[project:App]
type = exe

[config:Release|x64]
rtti = false
buffer_security_check = false
generate_manifest = false

[config:Custom|x64] : Template:Release
warning_level = Level3
)");
    const auto& custom = sol.projects[0].configurations.at("Custom|x64");
    CHECK_FALSE(custom.cl_compile.runtime_type_info);
    CHECK_FALSE(custom.cl_compile.buffer_security_check);
    CHECK_FALSE(custom.generate_manifest);
}

TEST_CASE("Explicit generate_manifest = true in a derived config beats a template false", "[bugfix3_parser]") {
    const Solution sol = bf3_parse(R"(
[solution]
configurations = Release, Custom
platforms = x64

[project:App]
type = exe

[config:Release|x64]
generate_manifest = false

[config:Custom|x64] : Template:Release
generate_manifest = true
)");
    CHECK(sol.projects[0].configurations.at("Custom|x64").generate_manifest);
}

TEST_CASE("An explicit generate_debug_info = false on Release survives the defaults", "[bugfix3_parser]") {
    const Solution sol = bf3_parse(R"(
[solution]
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
generate_debug_info[Release|x64] = false
link_incremental[Debug|x64] = false
)");
    const auto& cfgs = sol.projects[0].configurations;
    CHECK_FALSE(cfgs.at("Release|x64").link.generate_debug_info);
    CHECK_FALSE(cfgs.at("Debug|x64").link_incremental);
}
