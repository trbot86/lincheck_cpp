#include <lincheck/source_audit.hpp>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct Finding {
    std::string path;
    unsigned line = 0;
    unsigned column = 0;
    std::string kind;
    std::string token;
    std::string message;
};

struct Classification {
    lincheck::SourceAuditKind kind;
    std::string_view message;
};

std::string normalize_qualified_name(std::string name) {
    if (name.rfind("::", 0) == 0) {
        name.erase(0, 2);
    }
    constexpr std::string_view libcxx_inline_namespace = "std::__1::";
    if (name.rfind(libcxx_inline_namespace, 0) == 0) {
        name.replace(0, libcxx_inline_namespace.size(), "std::");
    }
    constexpr std::string_view libstdcxx_inline_namespace = "std::__cxx11::";
    if (name.rfind(libstdcxx_inline_namespace, 0) == 0) {
        name.replace(0, libstdcxx_inline_namespace.size(), "std::");
    }
    return name;
}

std::optional<Classification> classify_qualified_name(std::string_view name) {
    if (name == "std::atomic" || name == "std::atomic_ref") {
        return Classification{lincheck::SourceAuditKind::standard_atomic, "use lincheck atomic wrappers"};
    }
    if (name == "std::atomic_thread_fence" || name == "std::atomic_signal_fence") {
        return Classification{lincheck::SourceAuditKind::standard_atomic, "use lincheck fence helpers"};
    }
    if (name == "std::thread" || name == "std::jthread") {
        return Classification{lincheck::SourceAuditKind::standard_thread, "use lincheck thread wrappers"};
    }
    if (
        name == "std::mutex" ||
        name == "std::recursive_mutex" ||
        name == "std::timed_mutex" ||
        name == "std::recursive_timed_mutex" ||
        name == "std::shared_mutex" ||
        name == "std::shared_timed_mutex" ||
        name == "std::lock_guard" ||
        name == "std::unique_lock" ||
        name == "std::shared_lock" ||
        name == "std::scoped_lock"
    ) {
        return Classification{lincheck::SourceAuditKind::standard_mutex, "use lincheck lock wrappers"};
    }
    if (name == "std::condition_variable" || name == "std::condition_variable_any") {
        return Classification{
            lincheck::SourceAuditKind::standard_condition_variable,
            "use lincheck condition-variable wrappers"
        };
    }
    if (
        name == "std::binary_semaphore" ||
        name == "std::counting_semaphore" ||
        name == "std::barrier" ||
        name == "std::latch" ||
        name == "std::this_thread::yield" ||
        name == "std::this_thread::sleep_for" ||
        name == "std::this_thread::sleep_until"
    ) {
        return Classification{lincheck::SourceAuditKind::standard_wait, "use lincheck wait/scheduling wrappers"};
    }
    if (name.rfind("__atomic_", 0) == 0 || name.rfind("__sync_", 0) == 0) {
        return Classification{lincheck::SourceAuditKind::compiler_atomic_builtin, "wrap compiler atomic builtins"};
    }
    return std::nullopt;
}

class SourceLineCache {
public:
    std::string_view line(std::string_view path, unsigned line_number) {
        if (line_number == 0) return {};
        auto& lines = lines_by_path_[std::string(path)];
        if (!lines) {
            lines = read_lines(path);
        }
        if (line_number > lines->size()) return {};
        return (*lines)[line_number - 1];
    }

private:
    static std::vector<std::string> read_lines(std::string_view path) {
        std::ifstream input{std::string(path)};
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(std::move(line));
        }
        return lines;
    }

    std::unordered_map<std::string, std::optional<std::vector<std::string>>> lines_by_path_;
};

std::optional<std::string> raw_sync_record_name(clang::QualType type) {
    if (type.isNull()) return std::nullopt;

    clang::QualType current = type.getCanonicalType().getUnqualifiedType();
    while (true) {
        if (current->isReferenceType()) {
            current = current->getPointeeType().getCanonicalType().getUnqualifiedType();
            continue;
        }
        if (current->isPointerType()) {
            current = current->getPointeeType().getCanonicalType().getUnqualifiedType();
            continue;
        }
        if (const auto* array = current->getAsArrayTypeUnsafe()) {
            current = array->getElementType().getCanonicalType().getUnqualifiedType();
            continue;
        }
        break;
    }

    const auto* record = current->getAsCXXRecordDecl();
    if (record == nullptr) return std::nullopt;
    return normalize_qualified_name(record->getQualifiedNameAsString());
}

class RawSyncVisitor : public clang::RecursiveASTVisitor<RawSyncVisitor> {
public:
    RawSyncVisitor(
        clang::ASTContext& context,
        std::vector<Finding>& findings,
        SourceLineCache& source_lines,
        const lincheck::SourceAuditOptions& options
    )
        : context_(context), findings_(findings), source_lines_(source_lines), options_(options) {}

    bool VisitDeclaratorDecl(clang::DeclaratorDecl* decl) {
        if (
            !llvm::isa<clang::VarDecl>(decl) &&
            !llvm::isa<clang::FieldDecl>(decl) &&
            !llvm::isa<clang::ParmVarDecl>(decl)
        ) {
            return true;
        }
        auto name = raw_sync_record_name(decl->getType());
        if (!name) return true;
        auto classification = classify_qualified_name(*name);
        if (!classification) return true;
        add_finding(decl->getLocation(), *name, *classification);
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* expr) {
        const auto* callee = expr->getDirectCallee();
        if (callee == nullptr) return true;
        auto name = normalize_qualified_name(callee->getQualifiedNameAsString());
        auto classification = classify_qualified_name(name);
        if (!classification) return true;
        add_finding(expr->getExprLoc(), name, *classification);
        return true;
    }

private:
    void add_finding(clang::SourceLocation location, std::string token, Classification classification) {
        auto& source_manager = context_.getSourceManager();
        const auto spelling_location = source_manager.getSpellingLoc(location);
        if (!spelling_location.isValid() || !source_manager.isWrittenInMainFile(spelling_location)) {
            return;
        }
        const auto presumed = source_manager.getPresumedLoc(spelling_location);
        if (!presumed.isValid()) return;
        const std::string path = presumed.getFilename();
        const auto line = presumed.getLine();
        if (!lincheck::detail::source_audit_path_selected(path, options_)) {
            return;
        }
        if (lincheck::detail::source_audit_token_allowed(token, options_)) {
            return;
        }
        const auto source_line = source_lines_.line(path, line);
        if (lincheck::detail::source_audit_line_suppressed(source_line, options_)) {
            return;
        }
        if (
            line > 1 &&
            lincheck::detail::source_audit_suppresses_next_line(source_lines_.line(path, line - 1))
        ) {
            return;
        }
        findings_.push_back(Finding{
            .path = std::move(path),
            .line = line,
            .column = presumed.getColumn(),
            .kind = lincheck::source_audit_kind_name(classification.kind),
            .token = std::move(token),
            .message = std::string(classification.message)
        });
    }

    clang::ASTContext& context_;
    std::vector<Finding>& findings_;
    SourceLineCache& source_lines_;
    const lincheck::SourceAuditOptions& options_;
};

class RawSyncConsumer final : public clang::ASTConsumer {
public:
    RawSyncConsumer(
        clang::ASTContext& context,
        std::vector<Finding>& findings,
        SourceLineCache& source_lines,
        const lincheck::SourceAuditOptions& options
    )
        : visitor_(context, findings, source_lines, options) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        visitor_.TraverseDecl(context.getTranslationUnitDecl());
    }

private:
    RawSyncVisitor visitor_;
};

class RawSyncAction final : public clang::ASTFrontendAction {
public:
    RawSyncAction(
        std::vector<Finding>& findings,
        SourceLineCache& source_lines,
        const lincheck::SourceAuditOptions& options
    )
        : findings_(findings), source_lines_(source_lines), options_(options) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& compiler,
        llvm::StringRef
    ) override {
        return std::make_unique<RawSyncConsumer>(
            compiler.getASTContext(),
            findings_,
            source_lines_,
            options_
        );
    }

private:
    std::vector<Finding>& findings_;
    SourceLineCache& source_lines_;
    const lincheck::SourceAuditOptions& options_;
};

class RawSyncActionFactory final : public clang::tooling::FrontendActionFactory {
public:
    RawSyncActionFactory(
        std::vector<Finding>& findings,
        SourceLineCache& source_lines,
        const lincheck::SourceAuditOptions& options
    )
        : findings_(findings), source_lines_(source_lines), options_(options) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<RawSyncAction>(findings_, source_lines_, options_);
    }

private:
    std::vector<Finding>& findings_;
    SourceLineCache& source_lines_;
    const lincheck::SourceAuditOptions& options_;
};

llvm::cl::OptionCategory category("lincheck-clang-source-audit options");
llvm::cl::list<std::string> include_path_substrings(
    "include",
    llvm::cl::desc("Only audit paths containing TEXT. May be repeated."),
    llvm::cl::value_desc("TEXT"),
    llvm::cl::cat(category)
);
llvm::cl::list<std::string> exclude_path_substrings(
    "exclude",
    llvm::cl::desc("Skip paths containing TEXT. May be repeated."),
    llvm::cl::value_desc("TEXT"),
    llvm::cl::cat(category)
);
llvm::cl::list<std::string> allowed_token_substrings(
    "allow-token",
    llvm::cl::desc("Do not report matching raw API tokens containing TEXT. May be repeated."),
    llvm::cl::value_desc("TEXT"),
    llvm::cl::cat(category)
);
llvm::cl::list<std::string> allowed_line_substrings(
    "allow-line",
    llvm::cl::desc("Do not report source lines containing TEXT. May be repeated."),
    llvm::cl::value_desc("TEXT"),
    llvm::cl::cat(category)
);

lincheck::SourceAuditOptions source_audit_options() {
    lincheck::SourceAuditOptions options;
    options.include_path_substrings.assign(include_path_substrings.begin(), include_path_substrings.end());
    options.exclude_path_substrings.assign(exclude_path_substrings.begin(), exclude_path_substrings.end());
    options.allowed_token_substrings.assign(allowed_token_substrings.begin(), allowed_token_substrings.end());
    options.allowed_line_substrings.assign(allowed_line_substrings.begin(), allowed_line_substrings.end());
    return options;
}

void print_findings(const std::vector<Finding>& findings) {
    for (const auto& finding : findings) {
        llvm::outs()
            << finding.path << ":" << finding.line << ":" << finding.column
            << ": lincheck-clang-source-audit: " << finding.kind
            << " " << finding.token << ": " << finding.message << "\n";
    }
}

} // namespace

int main(int argc, const char** argv) {
    llvm::InitLLVM init(argc, argv);
    auto parser = clang::tooling::CommonOptionsParser::create(argc, argv, category);
    if (!parser) {
        llvm::logAllUnhandledErrors(parser.takeError(), llvm::errs(), "lincheck_clang_source_audit: ");
        return 2;
    }

    const auto options = source_audit_options();
    SourceLineCache source_lines;
    std::vector<Finding> findings;
    clang::tooling::ClangTool tool(parser->getCompilations(), parser->getSourcePathList());
    RawSyncActionFactory factory(findings, source_lines, options);
    const int tool_result = tool.run(&factory);
    if (tool_result != 0) {
        return tool_result;
    }
    if (!findings.empty()) {
        std::sort(findings.begin(), findings.end(), [](const auto& left, const auto& right) {
            if (left.path != right.path) return left.path < right.path;
            if (left.line != right.line) return left.line < right.line;
            if (left.column != right.column) return left.column < right.column;
            return left.token < right.token;
        });
        print_findings(findings);
        return 1;
    }
    return 0;
}
