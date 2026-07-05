#include <lincheck/source_audit.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
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

void collect_source_paths(const std::filesystem::path& path, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        out.push_back(path);
        return;
    }
    if (!std::filesystem::is_directory(path, ec)) {
        throw std::runtime_error("source audit path does not exist: " + path.string());
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
        path,
        std::filesystem::directory_options::skip_permission_denied
    )) {
        if (entry.is_regular_file(ec) && is_source_file(entry.path())) {
            out.push_back(entry.path());
        }
    }
}

void print_usage(std::ostream& out) {
    out
        << "usage: lincheck_source_audit [options] <file-or-directory>...\n"
        << "\n"
        << "Options:\n"
        << "  --include=TEXT       Only audit paths containing TEXT. May be repeated.\n"
        << "  --exclude=TEXT       Skip paths containing TEXT. May be repeated.\n"
        << "  --allow-token=TEXT   Do not report matching raw API tokens containing TEXT.\n"
        << "  --allow-line=TEXT    Do not report source lines containing TEXT.\n"
        << "  --help               Show this help text.\n"
        << "\n"
        << "Inline suppressions:\n"
        << "  // NOLINT(lincheck-raw-sync)\n"
        << "  // NOLINTNEXTLINE(lincheck-raw-sync)\n"
        << "  // lincheck: allow-raw-sync\n"
        << "  // lincheck: allow-next-raw-sync\n";
}

} // namespace

int main(int argc, char** argv) {
    lincheck::SourceAuditOptions options;
    std::vector<std::filesystem::path> roots;
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
        if (parse_options && has_prefix(arg, "--include=")) {
            options.include_path_substrings.push_back(arg.substr(std::string("--include=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--exclude=")) {
            options.exclude_path_substrings.push_back(arg.substr(std::string("--exclude=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--allow-token=")) {
            options.allowed_token_substrings.push_back(arg.substr(std::string("--allow-token=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "--allow-line=")) {
            options.allowed_line_substrings.push_back(arg.substr(std::string("--allow-line=").size()));
            continue;
        }
        if (parse_options && has_prefix(arg, "-")) {
            std::cerr << "unknown option: " << arg << "\n\n";
            print_usage(std::cerr);
            return 2;
        }
        roots.emplace_back(arg);
    }

    if (roots.empty()) {
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

        std::vector<lincheck::SourceAuditFinding> all_findings;
        for (const auto& file : files) {
            auto findings = lincheck::audit_source_file(file.string(), options);
            all_findings.insert(all_findings.end(), findings.begin(), findings.end());
        }

        if (!all_findings.empty()) {
            std::cout << lincheck::format_source_audit_report(all_findings);
            return 1;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "lincheck_source_audit: " << e.what() << '\n';
        return 2;
    }
}
