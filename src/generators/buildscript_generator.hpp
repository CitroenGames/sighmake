#pragma once

#include "common/generator.hpp"

namespace vcxproj {

class BuildscriptGenerator : public Generator {
public:
    BuildscriptGenerator() = default;

    bool generate(Solution& solution, const std::string& output_dir) override;

    std::string name() const override { return "buildscript"; }

    std::string description() const override {
        return "Sighmake buildscript generator";
    }
};

} // namespace vcxproj
