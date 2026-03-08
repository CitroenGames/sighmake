#include "pch.h"
#include "catch_amalgamated.hpp"
#include "common/toolset_registry.hpp"

using namespace vcxproj;

// ============================================================================
// resolve() tests
// ============================================================================

TEST_CASE("ToolsetRegistry resolves year-based names to toolset IDs", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.resolve("msvc2026") == "v145");
    CHECK(reg.resolve("msvc2022") == "v143");
    CHECK(reg.resolve("msvc2019") == "v142");
    CHECK(reg.resolve("msvc2017") == "v141");
}

TEST_CASE("ToolsetRegistry passes through bare year numbers", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    // Bare years are not in the toolchain map, so they pass through as-is
    CHECK(reg.resolve("2022") == "2022");
    CHECK(reg.resolve("2019") == "2019");
}

TEST_CASE("ToolsetRegistry passes through known toolset IDs", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.resolve("v143") == "v143");
    CHECK(reg.resolve("v142") == "v142");
    CHECK(reg.resolve("v141") == "v141");
}

TEST_CASE("ToolsetRegistry passes through unknown toolset IDs", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.resolve("v999") == "v999");
}

TEST_CASE("ToolsetRegistry resolve returns default for empty input", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    // Empty input returns the default toolset (which is "" initially but always has_value)
    auto result = reg.resolve("");
    CHECK(result.has_value());
}

// ============================================================================
// get_info() tests
// ============================================================================

TEST_CASE("ToolsetRegistry get_info returns info for known toolsets", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    auto info = reg.get_info("v143");
    REQUIRE(info.has_value());
    CHECK(info->id == "v143");
    CHECK(info->year == 2022);
    CHECK(info->is_legacy == false);
}

TEST_CASE("ToolsetRegistry get_info returns nullopt for unknown toolsets", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK_FALSE(reg.get_info("v999").has_value());
}

// ============================================================================
// is_known() tests
// ============================================================================

TEST_CASE("ToolsetRegistry is_known returns true for known toolsets", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.is_known("v143") == true);
    CHECK(reg.is_known("v142") == true);
    CHECK(reg.is_known("v141") == true);
}

TEST_CASE("ToolsetRegistry is_known returns false for unknown toolsets", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.is_known("v999") == false);
}

// ============================================================================
// get_toolset_year() tests
// ============================================================================

TEST_CASE("ToolsetRegistry get_toolset_year returns correct year", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.get_toolset_year("v143") == 2022);
    CHECK(reg.get_toolset_year("v142") == 2019);
    CHECK(reg.get_toolset_year("v141") == 2017);
}

TEST_CASE("ToolsetRegistry get_toolset_year returns 0 for unknown", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.get_toolset_year("v999") == 0);
}

// ============================================================================
// set_default / get_default tests
// ============================================================================

TEST_CASE("ToolsetRegistry set_default and get_default round-trip", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();

    // Save current default to restore later
    std::string original_default = reg.get_default();

    reg.set_default("v143");
    CHECK(reg.get_default() == "v143");

    reg.set_default("v142");
    CHECK(reg.get_default() == "v142");

    // Restore original
    reg.set_default(original_default);
}

// ============================================================================
// Legacy/older toolset tests
// ============================================================================

TEST_CASE("ToolsetRegistry resolves older MSVC versions", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    CHECK(reg.resolve("msvc2015") == "v140");
    CHECK(reg.resolve("msvc2013") == "v120");
    CHECK(reg.resolve("msvc2012") == "v110");
    CHECK(reg.resolve("msvc2010") == "v100");
}

TEST_CASE("ToolsetRegistry marks older toolsets as legacy", "[toolset]") {
    auto& reg = ToolsetRegistry::instance();
    auto info = reg.get_info("v100");
    if (info.has_value()) {
        CHECK(info->is_legacy == true);
    }
}
