#include "pch.h"
#include "generators/deps_exporter.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

namespace {

std::string config_type_label(const std::string& config_type) {
    if (config_type == "Application")    return "Executable";
    if (config_type == "StaticLibrary")  return "Static Library";
    if (config_type == "DynamicLibrary") return "Dynamic Library";
    if (config_type == "Utility")        return "Utility";
    return config_type.empty() ? "Unknown" : config_type;
}

std::string config_type_css_class(const std::string& config_type) {
    if (config_type == "Application")    return "exe";
    if (config_type == "StaticLibrary")  return "staticlib";
    if (config_type == "DynamicLibrary") return "dll";
    if (config_type == "Utility")        return "utility";
    return "unknown";
}

std::string escape_html(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&':  result += "&amp;";  break;
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&#39;";  break;
            default:   result += c;        break;
        }
    }
    return result;
}

std::string get_project_type(const Project& proj) {
    if (proj.configurations.empty()) return "";
    return proj.configurations.begin()->second.config_type;
}

void write_css(std::ofstream& out) {
    out << "<style>\n";
    out << R"(  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
         background: #f5f5f5; color: #333; padding: 2rem; line-height: 1.6; }
  h1 { font-size: 1.8rem; margin-bottom: 0.5rem; }
  h2 { font-size: 1.3rem; margin: 1.5rem 0 0.75rem; border-bottom: 2px solid #ddd; padding-bottom: 0.3rem; }
  .meta { color: #666; font-size: 0.9rem; margin-bottom: 1.5rem; }
  .project-cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 1rem; }
  .card { background: #fff; border-radius: 8px; padding: 1rem 1.25rem;
          box-shadow: 0 1px 3px rgba(0,0,0,0.1); border-left: 4px solid #ccc; }
  .card.exe       { border-left-color: #2196F3; }
  .card.staticlib { border-left-color: #4CAF50; }
  .card.dll       { border-left-color: #FF9800; }
  .card.utility   { border-left-color: #9C27B0; }
  .card h3 { font-size: 1.1rem; margin-bottom: 0.25rem; }
  .card .type-badge { display: inline-block; font-size: 0.75rem; padding: 0.1rem 0.5rem;
                      border-radius: 4px; color: #fff; margin-bottom: 0.5rem; }
  .badge-exe       { background: #2196F3; }
  .badge-staticlib { background: #4CAF50; }
  .badge-dll       { background: #FF9800; }
  .badge-utility   { background: #9C27B0; }
  .badge-unknown   { background: #999; }
  .dep-list { list-style: none; padding-left: 0; }
  .dep-list li { padding: 0.2rem 0; font-size: 0.9rem; }
  .dep-list li::before { content: '\2192'; margin-right: 0.4rem; color: #999; }
  .vis-tag { font-size: 0.7rem; padding: 0.05rem 0.35rem; border-radius: 3px;
             color: #fff; margin-left: 0.3rem; vertical-align: middle; }
  .vis-PUBLIC    { background: #2196F3; }
  .vis-PRIVATE   { background: #607D8B; }
  .vis-INTERFACE { background: #9C27B0; }
  .no-deps { color: #999; font-style: italic; font-size: 0.9rem; }
  .matrix-container { overflow-x: auto; margin: 1rem 0; }
  table.dep-matrix { border-collapse: collapse; font-size: 0.8rem; }
  table.dep-matrix th, table.dep-matrix td { border: 1px solid #ddd; padding: 0.3rem 0.5rem;
                                              text-align: center; min-width: 2.5rem; }
  table.dep-matrix th { background: #f0f0f0; font-weight: 600; white-space: nowrap; }
  table.dep-matrix th.row-header { text-align: right; }
  table.dep-matrix td.dep-pub   { background: #BBDEFB; }
  table.dep-matrix td.dep-priv  { background: #CFD8DC; }
  table.dep-matrix td.dep-iface { background: #E1BEE7; }
  table.dep-matrix td.dep-none  { background: #fff; }
  table.dep-matrix td.dep-self  { background: #eee; }
  .legend { display: flex; gap: 1.5rem; flex-wrap: wrap; margin: 0.75rem 0; font-size: 0.85rem; }
  .legend-item { display: flex; align-items: center; gap: 0.3rem; }
  .legend-swatch { width: 14px; height: 14px; border-radius: 3px; border: 1px solid #ccc; }
  footer { margin-top: 2rem; padding-top: 1rem; border-top: 1px solid #ddd;
           color: #999; font-size: 0.8rem; }
)";
    out << "</style>\n";
}

void write_project_cards(std::ofstream& out, const Solution& solution) {
    out << "<h2>Projects (" << solution.projects.size() << ")</h2>\n";
    out << "<div class=\"project-cards\">\n";

    for (const auto& proj : solution.projects) {
        std::string ptype = get_project_type(proj);
        std::string css_class = config_type_css_class(ptype);

        out << "  <div class=\"card " << css_class << "\">\n";
        out << "    <h3>" << escape_html(proj.name) << "</h3>\n";
        out << "    <span class=\"type-badge badge-" << css_class << "\">"
            << escape_html(config_type_label(ptype)) << "</span>\n";

        if (proj.project_references.empty()) {
            out << "    <p class=\"no-deps\">No dependencies</p>\n";
        } else {
            out << "    <ul class=\"dep-list\">\n";
            for (const auto& dep : proj.project_references) {
                std::string vis_str = visibility_to_string(dep.visibility);
                out << "      <li>" << escape_html(dep.name)
                    << " <span class=\"vis-tag vis-" << vis_str << "\">"
                    << vis_str << "</span></li>\n";
            }
            out << "    </ul>\n";
        }
        out << "  </div>\n";
    }

    out << "</div>\n";
}

void write_dependency_matrix(std::ofstream& out, const Solution& solution) {
    if (solution.projects.size() <= 1) return;

    out << "<h2>Dependency Matrix</h2>\n";
    out << "<div class=\"legend\">\n";
    out << "  <div class=\"legend-item\"><div class=\"legend-swatch\" style=\"background:#BBDEFB\"></div> PUBLIC</div>\n";
    out << "  <div class=\"legend-item\"><div class=\"legend-swatch\" style=\"background:#CFD8DC\"></div> PRIVATE</div>\n";
    out << "  <div class=\"legend-item\"><div class=\"legend-swatch\" style=\"background:#E1BEE7\"></div> INTERFACE</div>\n";
    out << "</div>\n";
    out << "<p style=\"font-size:0.85rem;color:#666;margin-bottom:0.5rem;\">Rows depend on columns.</p>\n";
    out << "<div class=\"matrix-container\">\n";
    out << "<table class=\"dep-matrix\">\n";

    // Header row
    out << "  <tr><th></th>";
    for (const auto& proj : solution.projects) {
        out << "<th>" << escape_html(proj.name) << "</th>";
    }
    out << "</tr>\n";

    // Data rows
    for (size_t row = 0; row < solution.projects.size(); ++row) {
        const auto& proj = solution.projects[row];
        out << "  <tr><th class=\"row-header\">" << escape_html(proj.name) << "</th>";

        // Build lookup for this project's dependencies
        std::map<std::string, DependencyVisibility> dep_map;
        for (const auto& dep : proj.project_references) {
            dep_map[dep.name] = dep.visibility;
        }

        for (size_t col = 0; col < solution.projects.size(); ++col) {
            if (row == col) {
                out << "<td class=\"dep-self\">&mdash;</td>";
            } else {
                const std::string& col_name = solution.projects[col].name;
                auto it = dep_map.find(col_name);
                if (it != dep_map.end()) {
                    std::string vis_str = visibility_to_string(it->second);
                    std::string css;
                    if (it->second == DependencyVisibility::PUBLIC)         css = "dep-pub";
                    else if (it->second == DependencyVisibility::PRIVATE)   css = "dep-priv";
                    else if (it->second == DependencyVisibility::INTERFACE) css = "dep-iface";
                    out << "<td class=\"" << css << "\">" << vis_str.substr(0, 3) << "</td>";
                } else {
                    out << "<td class=\"dep-none\"></td>";
                }
            }
        }
        out << "</tr>\n";
    }

    out << "</table>\n";
    out << "</div>\n";
}

} // anonymous namespace

bool export_dependencies_html(const Solution& solution, const std::string& output_dir) {
    fs::path out_path = fs::path(output_dir) / (solution.name + "_dependencies.html");

    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::cerr << "Error: Could not create dependency report: " << out_path.string() << "\n";
        return false;
    }

    // Count total dependencies
    size_t total_deps = 0;
    for (const auto& proj : solution.projects) {
        total_deps += proj.project_references.size();
    }

    out << "<!DOCTYPE html>\n";
    out << "<html lang=\"en\">\n";
    out << "<head>\n";
    out << "  <meta charset=\"UTF-8\">\n";
    out << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    out << "  <title>" << escape_html(solution.name) << " - Dependency Report</title>\n";
    write_css(out);
    out << "</head>\n";
    out << "<body>\n";

    // Header
    out << "<h1>" << escape_html(solution.name) << " &mdash; Dependency Report</h1>\n";
    out << "<p class=\"meta\">"
        << solution.projects.size() << " projects, "
        << total_deps << " dependencies";

    if (!solution.configurations.empty()) {
        out << " &bull; Configurations: ";
        for (size_t i = 0; i < solution.configurations.size(); ++i) {
            if (i > 0) out << ", ";
            out << escape_html(solution.configurations[i]);
        }
    }
    if (!solution.platforms.empty()) {
        out << " &bull; Platforms: ";
        for (size_t i = 0; i < solution.platforms.size(); ++i) {
            if (i > 0) out << ", ";
            out << escape_html(solution.platforms[i]);
        }
    }
    out << "</p>\n";

    write_project_cards(out, solution);
    write_dependency_matrix(out, solution);

    out << "<footer>Generated by sighmake --export-deps</footer>\n";
    out << "</body>\n";
    out << "</html>\n";

    out.close();

    std::cout << "Dependency report: " << out_path.string() << "\n";
    return true;
}

} // namespace vcxproj
