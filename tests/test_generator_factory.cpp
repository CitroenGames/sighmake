#include "pch.h"
#include "catch_amalgamated.hpp"
#include "common/generator.hpp"
#include "generators/vcxproj_generator.hpp"
#include "generators/makefile_generator.hpp"

using namespace vcxproj;

// ============================================================================
// GeneratorFactory tests
// ============================================================================

TEST_CASE("GeneratorFactory creates vcxproj generator", "[generator_factory]") {
    auto gen = GeneratorFactory::instance().create("vcxproj");
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "vcxproj");
}

TEST_CASE("GeneratorFactory creates makefile generator", "[generator_factory]") {
    auto gen = GeneratorFactory::instance().create("makefile");
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "makefile");
}

TEST_CASE("GeneratorFactory returns null for unknown generator", "[generator_factory]") {
    auto gen = GeneratorFactory::instance().create("nonexistent");
    CHECK(gen == nullptr);
}

TEST_CASE("GeneratorFactory has_generator", "[generator_factory]") {
    CHECK(GeneratorFactory::instance().has_generator("vcxproj") == true);
    CHECK(GeneratorFactory::instance().has_generator("makefile") == true);
    CHECK(GeneratorFactory::instance().has_generator("xyz") == false);
}

TEST_CASE("GeneratorFactory available_generators lists registered generators", "[generator_factory]") {
    auto generators = GeneratorFactory::instance().available_generators();
    bool has_vcxproj = false;
    bool has_makefile = false;
    for (const auto& name : generators) {
        if (name == "vcxproj") has_vcxproj = true;
        if (name == "makefile") has_makefile = true;
    }
    CHECK(has_vcxproj);
    CHECK(has_makefile);
}
