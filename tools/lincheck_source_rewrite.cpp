#include <lincheck/source_audit.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool has_prefix(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool is_source_file(const std::filesystem::path& path) {
    static const std::vector<std::string> extensions = {
        ".c", ".cc", ".cpp", ".cxx",
        ".h", ".hh", ".hpp", ".hxx",
        ".ipp", ".inl", ".tpp"
    };
    const auto extension = path.extension().string();
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool path_is_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

void collect_source_paths(const std::filesystem::path& path, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        out.push_back(path);
        return;
    }
    if (!std::filesystem::is_directory(path, ec)) {
        throw std::runtime_error("source rewrite path does not exist: " + path.string());
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
        path,
        std::filesystem::directory_options::skip_permission_denied
    )) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec) && is_source_file(entry.path())) {
            out.push_back(entry.path());
        }
    }
}

void print_usage(std::ostream& out) {
    out
        << "usage: lincheck_source_rewrite [options] <file-or-directory>...\n"
        << "\n"
        << "Options:\n"
        << "  --in-place           Rewrite source files in place.\n"
        << "  --output=FILE        Write rewritten source to FILE instead of stdout.\n"
        << "  --check              Exit with 1 when rewriting would change the file.\n"
        << "  --no-include         Do not add #include <lincheck/lincheck.hpp>.\n"
        << "  --include=TEXT       Only rewrite paths containing TEXT. May be repeated.\n"
        << "  --exclude=TEXT       Skip paths containing TEXT. May be repeated.\n"
        << "  --allow-token=TEXT   Do not rewrite matching raw API tokens containing TEXT.\n"
        << "  --allow-line=TEXT    Do not rewrite source lines containing TEXT.\n"
        << "  --help               Show this help text.\n"
        << "\n"
        << "Inline suppressions:\n"
        << "  // NOLINT(lincheck-raw-sync)\n"
        << "  // NOLINTNEXTLINE(lincheck-raw-sync)\n"
        << "  // lincheck: allow-raw-sync\n"
        << "  // lincheck: allow-next-raw-sync\n";
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to open output file: " + path.string());
    }
    output << text;
    if (!output) {
        throw std::runtime_error("failed to write output file: " + path.string());
    }
}

void print_report(
    std::ostream& out,
    const std::filesystem::path& path,
    const lincheck::SourceRewriteResult& result,
    bool include_file_for_added_include
) {
    if (result.added_lincheck_include && include_file_for_added_include) {
        out << path.string() << ": lincheck-source-rewrite: added #include <lincheck/lincheck.hpp>\n";
    } else if (result.added_lincheck_include) {
        out << "lincheck-source-rewrite: added #include <lincheck/lincheck.hpp>\n";
    }
    for (const auto& edit : result.edits) {
        out << lincheck::format_source_rewrite_edit(edit) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    lincheck::SourceRewriteOptions options;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path output_path;
    bool in_place = false;
    bool check_only = false;
    bool parse_options = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (parse_options && arg == "--") {
            parse_options = false;
            continue;
        }
        if (parse_options && (arg == "-h" || arg == "--help")) {
            print_usage(std::cout);
            return 0;
        }
        if (parse_options && arg == "--in-place") {
            in_place = true;
            continue;
        }
        if (parse_options && arg == "--check") {
            check_only = true;
            continue;
        }
        if (parse_options && arg == "--no-include") {
            options.add_lincheck_include = false;
            continue;
        }
        if (parse_options && has_prefix(arg, "--output=")) {
            output_path = arg.substr(std::string("--output=").size());
            continue;
        }
        if (parse_options && has_prefix(arg, "--include=")) {
            options.audit.include_path_substrings.push_back(arg.substr(std::string("--include=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--exclude=")) {
            options.audit.exclude_path_substrings.push_back(arg.substr(std::string("--exclude=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--allow-token=")) {
            options.audit.allowed_token_substrings.push_back(arg.substr(std::string("--allow-token=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--allow-line=")) {
            options.audit.allowed_line_substrings.push_back(arg.substr(std::string("--allow-line=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "-")) {
            std::cerr << "unknown option: " << arg << "\n\n";
            print_usage(std::cerr);
            return 2;
        }
        roots.emplace_back(arg);
    }

    if (roots.empty() || (in_place && !output_path.empty()) || (check_only && !output_path.empty()) || (check_only && in_place)) {
        print_usage(std::cerr);
        return 2;
    }

    try {
        std::vector<std::filesystem::path> files;
        for (const auto& root : roots) {
            collect_source_paths(root, files);
        }

        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());

        const bool explicit_single_file_input = roots.size() == 1 && path_is_regular_file(roots.front());
        if (!check_only && !in_place && output_path.empty() && !explicit_single_file_input) {
            std::cerr << "lincheck_source_rewrite: stdout mode requires exactly one source file input\n\n";
            print_usage(std::cerr);
            return 2;
        }
        if (!output_path.empty() && !explicit_single_file_input) {
            std::cerr << "lincheck_source_rewrite: --output requires exactly one source file input\n\n";
            print_usage(std::cerr);
            return 2;
        }

        std::vector<lincheck::SourceRewriteResult> results;
        results.reserve(files.size());
        for (const auto& file : files) {
            results.push_back(lincheck::rewrite_source_file(file.string(), options));
        }

        if (check_only) {
            bool changed = false;
            const bool include_file_for_added_include = files.size() > 1;
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (results[i].changed()) {
                    changed = true;
                    print_report(std::cerr, files[i], results[i], include_file_for_added_include);
                }
            }
            return changed ? 1 : 0;
        }

        if (in_place) {
            const bool include_file_for_added_include = files.size() > 1;
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (results[i].changed()) {
                    write_text(files[i], results[i].text);
                    print_report(std::cerr, files[i], results[i], include_file_for_added_include);
                }
            }
            return 0;
        }

        if (files.empty()) {
            std::cerr << "lincheck_source_rewrite: no source files found\n";
            return 2;
        }

        const auto& result = results.front();

        if (!output_path.empty()) {
            write_text(output_path, result.text);
            print_report(std::cerr, files.front(), result, false);
            return 0;
        }

        std::cout << result.text;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "lincheck_source_rewrite: " << e.what() << '\n';
        return 2;
    }
}
