#pragma once

#include "project_types.hpp"

namespace vcxproj {

// Abstract base class for project generators
class Generator {
public:
    virtual ~Generator() = default;

    // Generate project files for a solution
    // Returns true on success, false on failure
    virtual bool generate(Solution& solution, const std::string& output_dir) = 0;

    // Get the name of this generator (e.g., "vcxproj", "make", "cmake")
    virtual std::string name() const = 0;

    // Get a description of this generator
    virtual std::string description() const = 0;
};

// Generator factory for registering and creating generators
class GeneratorFactory {
public:
    using GeneratorCreator = std::function<std::unique_ptr<Generator>()>;

    // Get the singleton instance
    static GeneratorFactory& instance() {
        static GeneratorFactory factory;
        return factory;
    }

    // Register a generator
    void register_generator(const std::string& name, GeneratorCreator creator) {
        generators_[name] = creator;
    }

    // Create a generator by name
    std::unique_ptr<Generator> create(const std::string& name) const {
        auto it = generators_.find(name);
        if (it == generators_.end()) {
            return nullptr;
        }
        return it->second();
    }

    // Get list of available generator names
    std::vector<std::string> available_generators() const {
        std::vector<std::string> names;
        for (const auto& pair : generators_) {
            names.push_back(pair.first);
        }
        return names;
    }

    // Check if a generator exists
    bool has_generator(const std::string& name) const {
        return generators_.find(name) != generators_.end();
    }

private:
    GeneratorFactory() = default;
    std::map<std::string, GeneratorCreator> generators_;
};

// Helper class to auto-register generators
template<typename T>
class GeneratorRegistrar {
public:
    explicit GeneratorRegistrar(const std::string& name) {
        GeneratorFactory::instance().register_generator(name, []() {
            return std::make_unique<T>();
        });
    }
};

// Macro to easily register a generator
#define REGISTER_GENERATOR(ClassName, name) \
    static vcxproj::GeneratorRegistrar<ClassName> g_registrar_##ClassName(name);

} // namespace vcxproj