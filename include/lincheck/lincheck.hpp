#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace lincheck {

namespace detail {

template <typename T>
concept ValueEqualityComparable = requires(const T& a, const T& b) {
    { a == b } -> std::convertible_to<bool>;
};

template <typename T>
concept ValueStreamable = requires(std::ostream& out, const T& value) {
    out << value;
};

template <typename T>
concept ValueHashable = requires(const T& value) {
    { std::hash<T>{}(value) } -> std::convertible_to<std::size_t>;
};

template <typename T>
struct IsStdOptional : std::false_type {};

template <typename T>
struct IsStdOptional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_optional_v = IsStdOptional<std::decay_t<T>>::value;

template <typename T>
concept OptionalValuePayload =
    ValueEqualityComparable<T> &&
    ValueHashable<T> &&
    (ValueStreamable<T> || std::is_enum_v<T>);

struct CustomValueBase {
    virtual ~CustomValueBase() = default;
    virtual std::type_index type() const noexcept = 0;
    virtual std::string to_string() const = 0;
    virtual std::uint64_t stable_hash() const noexcept = 0;
    virtual bool equals(const CustomValueBase& other) const = 0;
};

template <typename T>
struct TypedCustomValueBase : CustomValueBase {
    virtual const T& value() const noexcept = 0;
};

template <typename Formatter, typename T>
std::string format_custom_value(Formatter& formatter, const T& value) {
    using Result = std::invoke_result_t<Formatter&, const T&>;
    if constexpr (std::is_convertible_v<Result, std::string>) {
        return std::string(formatter(value));
    } else {
        std::ostringstream out;
        out << formatter(value);
        return out.str();
    }
}

template <typename T, typename Formatter, typename Hasher, typename Equal>
class CustomValue final : public TypedCustomValueBase<T> {
public:
    CustomValue(T value, Formatter formatter, Hasher hasher, Equal equal)
        : value_(std::move(value)),
        formatter_(std::move(formatter)),
        hasher_(std::move(hasher)),
        equal_(std::move(equal)) {}

    std::type_index type() const noexcept override {
        return typeid(T);
    }

    const T& value() const noexcept override {
        return value_;
    }

    std::string to_string() const override {
        return format_custom_value(formatter_, value_);
    }

    std::uint64_t stable_hash() const noexcept override {
        try {
            return static_cast<std::uint64_t>(hasher_(value_));
        } catch (...) {
            return 0;
        }
    }

    bool equals(const CustomValueBase& other) const override {
        const auto* typed = dynamic_cast<const TypedCustomValueBase<T>*>(&other);
        return typed != nullptr && static_cast<bool>(equal_(value_, typed->value()));
    }

private:
    T value_;
    mutable Formatter formatter_;
    mutable Hasher hasher_;
    mutable Equal equal_;
};

struct OstreamFormatter {
    template <typename T>
    std::string operator()(const T& value) const {
        std::ostringstream out;
        out << value;
        return out.str();
    }
};

template <typename T>
struct StdHasher {
    std::uint64_t operator()(const T& value) const {
        return static_cast<std::uint64_t>(std::hash<T>{}(value));
    }
};

template <typename T>
struct OptionalFormatter {
    std::string operator()(const std::optional<T>& value) const {
        if (!value) return "optional(nullopt)";

        if constexpr (std::is_same_v<T, bool>) {
            return std::string("optional(") + (*value ? "true" : "false") + ")";
        } else if constexpr (std::is_enum_v<T>) {
            using Underlying = std::underlying_type_t<T>;
            return "optional(" + std::to_string(static_cast<Underlying>(*value)) + ")";
        } else {
            std::ostringstream out;
            out << "optional(" << *value << ")";
            return out.str();
        }
    }
};

template <typename T>
struct OptionalHasher {
    std::uint64_t operator()(const std::optional<T>& value) const {
        constexpr std::uint64_t absent = 0x6d4e0c3641f0a9f5ull;
        constexpr std::uint64_t present = 0x9e3779b97f4a7c15ull;
        if (!value) return absent;
        const auto hash = static_cast<std::uint64_t>(std::hash<T>{}(*value));
        return present ^ (hash + (present << 6) + (present >> 2));
    }
};

template <typename T>
using CustomValuePtr = std::shared_ptr<const T>;

template <typename T, typename Formatter, typename Hasher, typename Equal>
std::shared_ptr<const CustomValueBase> make_custom_value(
    T value,
    Formatter formatter,
    Hasher hasher,
    Equal equal
) {
    return std::make_shared<CustomValue<std::decay_t<T>, Formatter, Hasher, Equal>>(
        std::forward<T>(value),
        std::move(formatter),
        std::move(hasher),
        std::move(equal)
    );
}

} // namespace detail

class Value {
public:
    using CustomStorage = std::shared_ptr<const detail::CustomValueBase>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, CustomStorage>;

    Value() = default;
    Value(std::nullptr_t) : storage_(std::monostate{}) {}
    Value(bool value) : storage_(value) {}
    Value(const char* value) : storage_(std::string(value)) {}
    Value(std::string value) : storage_(std::move(value)) {}

    template <typename T>
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool> && std::is_signed_v<T>)
    Value(T value) : storage_(static_cast<std::int64_t>(value)) {}

    template <typename T>
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool> && std::is_unsigned_v<T>)
    Value(T value) : storage_(static_cast<std::uint64_t>(value)) {}

    template <typename T>
        requires std::is_floating_point_v<T>
    Value(T value) : storage_(static_cast<double>(value)) {}

    template <typename T>
        requires std::is_enum_v<T>
    Value(T value) : Value(static_cast<std::underlying_type_t<T>>(value)) {}

    template <typename T>
        requires detail::OptionalValuePayload<std::decay_t<T>>
    Value(std::optional<T> value)
        : storage_(detail::make_custom_value(
            std::optional<std::decay_t<T>>(std::move(value)),
            detail::OptionalFormatter<std::decay_t<T>>{},
            detail::OptionalHasher<std::decay_t<T>>{},
            std::equal_to<std::optional<std::decay_t<T>>>{}
        )) {}

    template <typename T>
        requires (
            !std::is_same_v<std::decay_t<T>, Value> &&
            !std::is_same_v<std::decay_t<T>, std::nullptr_t> &&
            !std::is_same_v<std::decay_t<T>, bool> &&
            !std::is_arithmetic_v<std::decay_t<T>> &&
            !std::is_enum_v<std::decay_t<T>> &&
            !detail::is_std_optional_v<std::decay_t<T>> &&
            !std::is_convertible_v<T, std::string> &&
            detail::ValueEqualityComparable<std::decay_t<T>> &&
            detail::ValueStreamable<std::decay_t<T>> &&
            detail::ValueHashable<std::decay_t<T>>
        )
    explicit Value(T&& value)
        : storage_(detail::make_custom_value(
            std::forward<T>(value),
            detail::OstreamFormatter{},
            detail::StdHasher<std::decay_t<T>>{},
            std::equal_to<std::decay_t<T>>{}
        )) {}

    template <typename T, typename Formatter, typename Hasher, typename Equal>
    static Value custom(T value, Formatter formatter, Hasher hasher, Equal equal) {
        Value result;
        result.storage_ = detail::make_custom_value(
            std::move(value),
            std::move(formatter),
            std::move(hasher),
            std::move(equal)
        );
        return result;
    }

    template <typename T, typename Formatter, typename Hasher>
        requires detail::ValueEqualityComparable<std::decay_t<T>>
    static Value custom(T value, Formatter formatter, Hasher hasher) {
        return custom(
            std::move(value),
            std::move(formatter),
            std::move(hasher),
            std::equal_to<std::decay_t<T>>{}
        );
    }

    const Storage& storage() const { return storage_; }

    std::string to_string() const {
        return std::visit([](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "void";
            } else if constexpr (std::is_same_v<T, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return value;
            } else if constexpr (std::is_same_v<T, CustomStorage>) {
                return value ? value->to_string() : "<custom-null>";
            } else {
                return std::to_string(value);
            }
        }, storage_);
    }

    std::uint64_t stable_hash() const noexcept {
        constexpr std::uint64_t offset = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;
        std::uint64_t hash = offset;

        auto add_byte = [&](std::uint8_t value) {
            hash ^= value;
            hash *= prime;
        };
        auto add_u64 = [&](std::uint64_t value) {
            for (int i = 0; i < 8; ++i) {
                add_byte(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
            }
        };

        add_u64(static_cast<std::uint64_t>(storage_.index()));
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return;
            } else if constexpr (std::is_same_v<T, bool>) {
                add_byte(value ? 1u : 0u);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                add_u64(static_cast<std::uint64_t>(value));
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                add_u64(value);
            } else if constexpr (std::is_same_v<T, double>) {
                static_assert(sizeof(double) == sizeof(std::uint64_t));
                std::uint64_t bits = 0;
                auto bytes = reinterpret_cast<const unsigned char*>(&value);
                for (std::size_t i = 0; i < sizeof(double); ++i) {
                    bits |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
                }
                add_u64(bits);
            } else if constexpr (std::is_same_v<T, std::string>) {
                add_u64(static_cast<std::uint64_t>(value.size()));
                for (unsigned char c : value) add_byte(c);
            } else if constexpr (std::is_same_v<T, CustomStorage>) {
                add_u64(value ? value->stable_hash() : 0);
            }
        }, storage_);

        return hash;
    }

    friend bool operator==(const Value& left, const Value& right) {
        if (left.storage_.index() != right.storage_.index()) return false;
        if (auto left_custom = std::get_if<CustomStorage>(&left.storage_)) {
            const auto* right_custom = std::get_if<CustomStorage>(&right.storage_);
            if (right_custom == nullptr) return false;
            if (!*left_custom || !*right_custom) return *left_custom == *right_custom;
            return (*left_custom)->equals(**right_custom);
        }
        return left.storage_ == right.storage_;
    }

private:
    Storage storage_;
};

struct ValueHash {
    std::size_t operator()(const Value& value) const noexcept {
        return static_cast<std::size_t>(value.stable_hash());
    }
};

struct OperationException {
    std::string type;
    std::string message;

    friend bool operator==(const OperationException&, const OperationException&) = default;
};

inline std::ostream& operator<<(std::ostream& out, const OperationException& exception) {
    out << "exception(" << exception.type;
    if (!exception.message.empty()) out << ": " << exception.message;
    return out << ")";
}

namespace detail {

inline std::uint64_t combine_string_hashes(const std::string& first, const std::string& second) {
    auto hash = static_cast<std::uint64_t>(std::hash<std::string>{}(first));
    const auto mix = static_cast<std::uint64_t>(std::hash<std::string>{}(second));
    hash ^= mix + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

inline OperationException describe_exception(const std::exception_ptr& ptr) {
    if (!ptr) return OperationException{.type = "unknown", .message = "unknown exception"};
    try {
        std::rethrow_exception(ptr);
    } catch (const std::exception& e) {
        return OperationException{.type = typeid(e).name(), .message = e.what()};
    } catch (...) {
        return OperationException{.type = "non-std", .message = "non-std exception"};
    }
}

inline Value exception_result_value(const std::exception_ptr& ptr) {
    return Value::custom(
        describe_exception(ptr),
        [](const OperationException& exception) {
            std::ostringstream out;
            out << exception;
            return out.str();
        },
        [](const OperationException& exception) {
            return combine_string_hashes(exception.type, exception.message);
        },
        std::equal_to<OperationException>{}
    );
}

} // namespace detail

template <typename T>
T value_cast(const Value& value) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, Value>) {
        return value;
    } else if constexpr (std::is_same_v<D, bool>) {
        if (auto p = std::get_if<bool>(&value.storage())) return *p;
    } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
        if (auto p = std::get_if<std::int64_t>(&value.storage())) return static_cast<D>(*p);
        if (auto p = std::get_if<std::uint64_t>(&value.storage())) return static_cast<D>(*p);
    } else if constexpr (std::is_integral_v<D> && std::is_unsigned_v<D>) {
        if (auto p = std::get_if<std::uint64_t>(&value.storage())) return static_cast<D>(*p);
        if (auto p = std::get_if<std::int64_t>(&value.storage())) return static_cast<D>(*p);
    } else if constexpr (std::is_floating_point_v<D>) {
        if (auto p = std::get_if<double>(&value.storage())) return static_cast<D>(*p);
    } else if constexpr (std::is_same_v<D, std::string>) {
        if (auto p = std::get_if<std::string>(&value.storage())) return *p;
    } else if constexpr (std::is_enum_v<D>) {
        using U = std::underlying_type_t<D>;
        return static_cast<D>(value_cast<U>(value));
    } else {
        if (auto p = std::get_if<Value::CustomStorage>(&value.storage())) {
            if (*p) {
                if (auto typed = std::dynamic_pointer_cast<const detail::TypedCustomValueBase<D>>(*p)) {
                    return typed->value();
                }
            }
        }
    }
    throw std::bad_variant_access();
}

template <typename T>
Value to_value(T&& value) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, Value>) {
        return std::forward<T>(value);
    } else {
        return Value(std::forward<T>(value));
    }
}

template <typename T>
std::string trace_value_string(const T& value) {
    using D = std::decay_t<T>;
    if constexpr (std::is_constructible_v<Value, D>) {
        return Value(value).to_string();
    } else {
        return "<value>";
    }
}

struct SourceLocation {
    const char* file = "";
    int line = 0;
    const char* function = "";
    const char* label = "";

    std::string to_string() const {
        std::ostringstream out;
        out << file << ":" << line;
        if (function != nullptr && function[0] != '\0') {
            out << " " << function;
        }
        if (label != nullptr && label[0] != '\0') {
            out << " " << label;
        }
        return out.str();
    }
};

inline SourceLocation source_location(const char* file, int line, const char* function, const char* label = "") {
    return SourceLocation{file, line, function, label};
}

class TraceFilter {
public:
    TraceFilter& include(std::string pattern) {
        include_patterns_.push_back(std::move(pattern));
        return *this;
    }

    TraceFilter& exclude(std::string pattern) {
        exclude_patterns_.push_back(std::move(pattern));
        return *this;
    }

    bool accepts(const std::string& line) const {
        const bool included = include_patterns_.empty() ||
            std::any_of(include_patterns_.begin(), include_patterns_.end(), [&](const std::string& pattern) {
                return line.find(pattern) != std::string::npos;
            });
        if (!included) return false;

        return std::none_of(exclude_patterns_.begin(), exclude_patterns_.end(), [&](const std::string& pattern) {
            return line.find(pattern) != std::string::npos;
        });
    }

    const std::vector<std::string>& include_patterns() const {
        return include_patterns_;
    }

    const std::vector<std::string>& exclude_patterns() const {
        return exclude_patterns_;
    }

private:
    std::vector<std::string> include_patterns_;
    std::vector<std::string> exclude_patterns_;
};

enum class SourceAuditKind {
    standard_atomic,
    standard_thread,
    standard_mutex,
    standard_condition_variable,
    standard_wait,
    platform_sync,
    compiler_atomic_builtin
};

struct SourceAuditFinding {
    std::string path;
    std::size_t line = 0;
    std::size_t column = 0;
    SourceAuditKind kind = SourceAuditKind::standard_atomic;
    std::string token;
    std::string message;
};

struct SourceAuditOptions {
    std::vector<std::string> include_path_substrings;
    std::vector<std::string> exclude_path_substrings;
    std::vector<std::string> allowed_token_substrings;
    std::vector<std::string> allowed_line_substrings;
};

struct SourceRewriteEdit {
    std::string path;
    std::size_t line = 0;
    std::size_t column = 0;
    std::string token;
    std::string replacement;
};

struct SourceRewriteOptions {
    SourceAuditOptions audit;
    bool add_lincheck_include = true;
};

struct SourceRewriteResult {
    std::string text;
    std::vector<SourceRewriteEdit> edits;
    bool added_lincheck_include = false;

    bool changed() const {
        return added_lincheck_include || !edits.empty();
    }
};

inline const char* source_audit_kind_name(SourceAuditKind kind) {
    switch (kind) {
        case SourceAuditKind::standard_atomic: return "standard_atomic";
        case SourceAuditKind::standard_thread: return "standard_thread";
        case SourceAuditKind::standard_mutex: return "standard_mutex";
        case SourceAuditKind::standard_condition_variable: return "standard_condition_variable";
        case SourceAuditKind::standard_wait: return "standard_wait";
        case SourceAuditKind::platform_sync: return "platform_sync";
        case SourceAuditKind::compiler_atomic_builtin: return "compiler_atomic_builtin";
    }
    return "unknown";
}

namespace detail {

struct SourceAuditPattern {
    std::string_view token;
    SourceAuditKind kind;
    std::string_view replacement;
    bool require_right_boundary = true;
};

struct SourceRewritePattern {
    std::string_view token;
    std::string_view replacement;
    bool require_right_boundary = true;
};

inline const std::vector<SourceAuditPattern>& source_audit_patterns() {
    static const std::vector<SourceAuditPattern> patterns = {
        {"std::condition_variable_any", SourceAuditKind::standard_condition_variable, "use lincheck::condition_variable or an explicit adapter"},
        {"std::condition_variable", SourceAuditKind::standard_condition_variable, "use lincheck::condition_variable"},
        {"std::recursive_timed_mutex", SourceAuditKind::standard_mutex, "use lincheck::recursive_timed_mutex"},
        {"std::shared_timed_mutex", SourceAuditKind::standard_mutex, "use lincheck::shared_timed_mutex"},
        {"std::recursive_mutex", SourceAuditKind::standard_mutex, "use lincheck::recursive_mutex"},
        {"std::timed_mutex", SourceAuditKind::standard_mutex, "use lincheck::timed_mutex"},
        {"std::shared_mutex", SourceAuditKind::standard_mutex, "use lincheck::shared_mutex"},
        {"std::shared_lock", SourceAuditKind::standard_mutex, "use lincheck::shared_lock"},
        {"std::atomic_thread_fence", SourceAuditKind::standard_atomic, "use lincheck::atomic_thread_fence"},
        {"std::atomic_signal_fence", SourceAuditKind::standard_atomic, "use lincheck::atomic_signal_fence"},
        {"std::this_thread::sleep_until", SourceAuditKind::standard_wait, "wrap waits with lincheck switch points or an explicit adapter"},
        {"std::this_thread::sleep_for", SourceAuditKind::standard_wait, "wrap waits with lincheck switch points or an explicit adapter"},
        {"std::this_thread::yield", SourceAuditKind::standard_wait, "use lincheck::this_thread::yield or LC_YIELD"},
        {"std::counting_semaphore", SourceAuditKind::standard_wait, "use lincheck::counting_semaphore"},
        {"std::binary_semaphore", SourceAuditKind::standard_wait, "use lincheck::binary_semaphore"},
        {"std::barrier", SourceAuditKind::standard_wait, "use lincheck::barrier"},
        {"std::latch", SourceAuditKind::standard_wait, "use lincheck::latch"},
        {"std::atomic_ref", SourceAuditKind::standard_atomic, "use lincheck::atomic_ref<T>"},
        {"std::atomic", SourceAuditKind::standard_atomic, "use lincheck::atomic<T>"},
        {"std::jthread", SourceAuditKind::standard_thread, "use lincheck::jthread"},
        {"std::thread", SourceAuditKind::standard_thread, "use lincheck::thread"},
        {"std::mutex", SourceAuditKind::standard_mutex, "use lincheck::mutex"},
        {"pthread_mutex", SourceAuditKind::platform_sync, "wrap pthread mutex calls with a Lincheck adapter", false},
        {"pthread_cond", SourceAuditKind::platform_sync, "wrap pthread condition variables with a Lincheck adapter", false},
        {"pthread_rwlock", SourceAuditKind::platform_sync, "wrap pthread rwlocks with a Lincheck adapter", false},
        {"pthread_create", SourceAuditKind::platform_sync, "use lincheck::thread or a thread adapter"},
        {"SYS_futex", SourceAuditKind::platform_sync, "wrap futex waits and wakes with a Lincheck adapter"},
        {"futex", SourceAuditKind::platform_sync, "wrap futex waits and wakes with a Lincheck adapter"},
        {"__atomic_", SourceAuditKind::compiler_atomic_builtin, "wrap compiler atomic builtins with Lincheck instrumentation", false},
        {"__sync_", SourceAuditKind::compiler_atomic_builtin, "wrap compiler sync builtins with Lincheck instrumentation", false}
    };
    return patterns;
}

inline const std::vector<SourceRewritePattern>& source_rewrite_patterns() {
    static const std::vector<SourceRewritePattern> patterns = {
        {"std::atomic_thread_fence", "lincheck::atomic_thread_fence"},
        {"std::atomic_signal_fence", "lincheck::atomic_signal_fence"},
        {"std::this_thread::sleep_until", "lincheck::this_thread::sleep_until"},
        {"std::this_thread::sleep_for", "lincheck::this_thread::sleep_for"},
        {"std::this_thread::yield", "lincheck::this_thread::yield"},
        {"std::counting_semaphore", "lincheck::counting_semaphore"},
        {"std::binary_semaphore", "lincheck::binary_semaphore"},
        {"std::barrier", "lincheck::barrier"},
        {"std::latch", "lincheck::latch"},
        {"std::recursive_timed_mutex", "lincheck::recursive_timed_mutex"},
        {"std::shared_timed_mutex", "lincheck::shared_timed_mutex"},
        {"std::recursive_mutex", "lincheck::recursive_mutex"},
        {"std::timed_mutex", "lincheck::timed_mutex"},
        {"std::shared_mutex", "lincheck::shared_mutex"},
        {"std::lock_guard<std::mutex>", "lincheck::lock_guard"},
        {"std::unique_lock<std::mutex>", "lincheck::unique_lock"},
        {"std::scoped_lock<std::mutex>", "lincheck::scoped_lock<lincheck::mutex>"},
        {"std::condition_variable", "lincheck::condition_variable"},
        {"std::lock_guard", "lincheck::lock_guard"},
        {"std::unique_lock", "lincheck::unique_lock"},
        {"std::shared_lock", "lincheck::shared_lock"},
        {"std::scoped_lock", "lincheck::scoped_lock"},
        {"std::atomic_ref", "lincheck::atomic_ref"},
        {"std::atomic", "lincheck::atomic"},
        {"std::jthread", "lincheck::jthread"},
        {"std::thread", "lincheck::thread"},
        {"std::mutex", "lincheck::mutex"}
    };
    return patterns;
}

inline bool source_audit_contains_any(std::string_view value, const std::vector<std::string>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& needle) {
        return !needle.empty() && value.find(needle) != std::string_view::npos;
    });
}

inline bool source_audit_path_selected(std::string_view path, const SourceAuditOptions& options) {
    if (
        !options.include_path_substrings.empty() &&
        !source_audit_contains_any(path, options.include_path_substrings)
    ) {
        return false;
    }
    return !source_audit_contains_any(path, options.exclude_path_substrings);
}

inline bool source_audit_identifier_char(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

inline bool source_audit_matches_token_boundary(
    std::string_view line,
    std::size_t position,
    std::size_t length,
    bool require_right_boundary
) {
    if (position > 0 && source_audit_identifier_char(line[position - 1])) {
        return false;
    }
    const auto right = position + length;
    return !require_right_boundary ||
        right >= line.size() ||
        !source_audit_identifier_char(line[right]);
}

inline bool source_audit_line_suppressed(std::string_view line, const SourceAuditOptions& options) {
    return line.find("NOLINT(lincheck-raw-sync)") != std::string_view::npos ||
        line.find("lincheck: allow-raw-sync") != std::string_view::npos ||
        source_audit_contains_any(line, options.allowed_line_substrings);
}

inline bool source_audit_suppresses_next_line(std::string_view line) {
    return line.find("NOLINTNEXTLINE(lincheck-raw-sync)") != std::string_view::npos ||
        line.find("lincheck: allow-next-raw-sync") != std::string_view::npos;
}

inline bool source_audit_token_allowed(std::string_view token, const SourceAuditOptions& options) {
    return source_audit_contains_any(token, options.allowed_token_substrings);
}

inline bool source_rewrite_has_lincheck_include(const std::string& source) {
    return source.find("#include <lincheck/lincheck.hpp>") != std::string::npos ||
        source.find("#include \"lincheck/lincheck.hpp\"") != std::string::npos;
}

inline void source_rewrite_prepend_lincheck_include(SourceRewriteResult& result) {
    result.text = "#include <lincheck/lincheck.hpp>\n" + result.text;
    result.added_lincheck_include = true;
}

inline std::string source_audit_sanitize_line(std::string_view line, bool& in_block_comment) {
    std::string sanitized(line.size(), ' ');
    std::size_t i = 0;
    while (i < line.size()) {
        if (in_block_comment) {
            if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/') {
                in_block_comment = false;
                i += 2;
            } else {
                ++i;
            }
            continue;
        }

        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
            break;
        }
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
            in_block_comment = true;
            i += 2;
            continue;
        }
        if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i++];
            while (i < line.size()) {
                if (line[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (line[i++] == quote) {
                    break;
                }
            }
            continue;
        }

        sanitized[i] = line[i];
        ++i;
    }
    return sanitized;
}

} // namespace detail

inline std::vector<SourceAuditFinding> audit_source_text(
    std::string path,
    const std::string& source,
    const SourceAuditOptions& options = {}
) {
    std::vector<SourceAuditFinding> findings;
    if (!detail::source_audit_path_selected(path, options)) {
        return findings;
    }

    std::istringstream input(source);
    std::string original_line;
    std::size_t line_number = 0;
    bool in_block_comment = false;
    bool suppress_next_line = false;

    while (std::getline(input, original_line)) {
        ++line_number;
        if (!original_line.empty() && original_line.back() == '\r') {
            original_line.pop_back();
        }

        const bool suppressed = suppress_next_line || detail::source_audit_line_suppressed(original_line, options);
        suppress_next_line = detail::source_audit_suppresses_next_line(original_line);
        const auto sanitized = detail::source_audit_sanitize_line(original_line, in_block_comment);
        if (suppressed) {
            continue;
        }

        for (const auto& pattern : detail::source_audit_patterns()) {
            if (detail::source_audit_token_allowed(pattern.token, options)) {
                continue;
            }

            std::size_t position = 0;
            while ((position = sanitized.find(pattern.token, position)) != std::string::npos) {
                if (
                    detail::source_audit_matches_token_boundary(
                        sanitized,
                        position,
                        pattern.token.size(),
                        pattern.require_right_boundary
                    )
                ) {
                    findings.push_back(SourceAuditFinding{
                        .path = path,
                        .line = line_number,
                        .column = position + 1,
                        .kind = pattern.kind,
                        .token = std::string(pattern.token),
                        .message = "raw synchronization API is not scheduled by Lincheck; " + std::string(pattern.replacement)
                    });
                }
                position += pattern.token.size();
            }
        }
    }

    return findings;
}

inline std::vector<SourceAuditFinding> audit_source_file(
    const std::string& path,
    const SourceAuditOptions& options = {}
) {
    if (!detail::source_audit_path_selected(path, options)) {
        return {};
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open source file for audit: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return audit_source_text(path, buffer.str(), options);
}

inline SourceRewriteResult rewrite_source_text(
    std::string path,
    const std::string& source,
    const SourceRewriteOptions& options = {}
) {
    SourceRewriteResult result;
    result.text = source;
    if (!detail::source_audit_path_selected(path, options.audit)) {
        return result;
    }

    std::istringstream input(source);
    std::ostringstream rewritten;
    std::string original_line;
    std::size_t line_number = 0;
    bool in_block_comment = false;
    bool suppress_next_line = false;
    bool first_output_line = true;

    while (std::getline(input, original_line)) {
        ++line_number;
        if (!original_line.empty() && original_line.back() == '\r') {
            original_line.pop_back();
        }

        const bool suppressed = suppress_next_line || detail::source_audit_line_suppressed(original_line, options.audit);
        suppress_next_line = detail::source_audit_suppresses_next_line(original_line);
        const auto sanitized = detail::source_audit_sanitize_line(original_line, in_block_comment);
        std::string rewritten_line = original_line;

        if (!suppressed) {
            struct PendingEdit {
                std::size_t position = 0;
                std::string_view token;
                std::string_view replacement;
            };

            std::vector<PendingEdit> pending;
            std::vector<bool> occupied(sanitized.size(), false);

            for (const auto& pattern : detail::source_rewrite_patterns()) {
                if (detail::source_audit_token_allowed(pattern.token, options.audit)) {
                    continue;
                }

                std::size_t position = 0;
                while ((position = sanitized.find(pattern.token, position)) != std::string::npos) {
                    const auto length = pattern.token.size();
                    const bool overlaps = std::any_of(
                        occupied.begin() + static_cast<std::ptrdiff_t>(position),
                        occupied.begin() + static_cast<std::ptrdiff_t>(std::min(position + length, occupied.size())),
                        [](bool value) { return value; }
                    );
                    if (
                        !overlaps &&
                        detail::source_audit_matches_token_boundary(
                            sanitized,
                            position,
                            length,
                            pattern.require_right_boundary
                        )
                    ) {
                        pending.push_back(PendingEdit{
                            .position = position,
                            .token = pattern.token,
                            .replacement = pattern.replacement
                        });
                        for (std::size_t i = position; i < position + length && i < occupied.size(); ++i) {
                            occupied[i] = true;
                        }
                    }
                    position += length;
                }
            }

            std::sort(pending.begin(), pending.end(), [](const PendingEdit& left, const PendingEdit& right) {
                return left.position > right.position;
            });
            for (const auto& edit : pending) {
                rewritten_line.replace(edit.position, edit.token.size(), edit.replacement);
                result.edits.push_back(SourceRewriteEdit{
                    .path = path,
                    .line = line_number,
                    .column = edit.position + 1,
                    .token = std::string(edit.token),
                    .replacement = std::string(edit.replacement)
                });
            }
        }

        if (!first_output_line) {
            rewritten << '\n';
        }
        first_output_line = false;
        rewritten << rewritten_line;
    }

    if (!source.empty() && source.back() == '\n') {
        rewritten << '\n';
    }

    result.text = rewritten.str();
    if (options.add_lincheck_include && !result.edits.empty() && !detail::source_rewrite_has_lincheck_include(result.text)) {
        detail::source_rewrite_prepend_lincheck_include(result);
    }
    return result;
}

inline SourceRewriteResult rewrite_source_file(
    const std::string& path,
    const SourceRewriteOptions& options = {}
) {
    if (!detail::source_audit_path_selected(path, options.audit)) {
        std::ifstream skipped_input(path);
        if (!skipped_input) {
            throw std::runtime_error("failed to open source file for rewrite: " + path);
        }
        std::ostringstream skipped_buffer;
        skipped_buffer << skipped_input.rdbuf();
        SourceRewriteResult result;
        result.text = skipped_buffer.str();
        return result;
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open source file for rewrite: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return rewrite_source_text(path, buffer.str(), options);
}

inline std::string format_source_rewrite_edit(const SourceRewriteEdit& edit) {
    std::ostringstream out;
    out << edit.path << ":" << edit.line << ":" << edit.column
        << ": lincheck-source-rewrite: replaced " << edit.token
        << " with " << edit.replacement;
    return out.str();
}

inline std::string format_source_rewrite_report(const SourceRewriteResult& result) {
    std::ostringstream out;
    if (result.added_lincheck_include) {
        out << "lincheck-source-rewrite: added #include <lincheck/lincheck.hpp>\n";
    }
    for (const auto& edit : result.edits) {
        out << format_source_rewrite_edit(edit) << '\n';
    }
    return out.str();
}

inline std::string format_source_audit_finding(const SourceAuditFinding& finding) {
    std::ostringstream out;
    out << finding.path << ":" << finding.line << ":" << finding.column
        << ": lincheck-source-audit: " << source_audit_kind_name(finding.kind)
        << ": " << finding.message
        << " [token: " << finding.token << "]";
    return out.str();
}

inline std::string format_source_audit_report(const std::vector<SourceAuditFinding>& findings) {
    std::ostringstream out;
    for (const auto& finding : findings) {
        out << format_source_audit_finding(finding) << '\n';
    }
    return out.str();
}

inline std::string pointer_to_string(const void* address) {
    std::ostringstream out;
    out << address;
    return out.str();
}

inline std::string stable_object_id(const void* address) {
    static std::mutex mutex;
    static std::unordered_map<const void*, std::size_t> ids;
    static std::size_t next_id = 1;

    std::lock_guard lock(mutex);
    auto [it, inserted] = ids.emplace(address, next_id);
    if (inserted) ++next_id;
    return "obj#" + std::to_string(it->second);
}

inline std::string stable_location_id(const SourceLocation& source) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::size_t> ids;
    static std::size_t next_id = 1;

    std::lock_guard lock(mutex);
    auto [it, inserted] = ids.emplace(source.to_string(), next_id);
    if (inserted) ++next_id;
    return "loc#" + std::to_string(it->second);
}

inline const char* memory_order_name(std::memory_order order) {
    switch (order) {
        case std::memory_order_relaxed: return "relaxed";
        case std::memory_order_consume: return "consume";
        case std::memory_order_acquire: return "acquire";
        case std::memory_order_release: return "release";
        case std::memory_order_acq_rel: return "acq_rel";
        case std::memory_order_seq_cst: return "seq_cst";
    }
    return "unknown";
}

inline std::memory_order compare_exchange_failure_order(std::memory_order order) {
    switch (order) {
        case std::memory_order_release:
            return std::memory_order_relaxed;
        case std::memory_order_acq_rel:
            return std::memory_order_acquire;
        default:
            return order;
    }
}

inline void validate_known_memory_order(std::memory_order order, std::string_view operation) {
    switch (order) {
        case std::memory_order_relaxed:
        case std::memory_order_consume:
        case std::memory_order_acquire:
        case std::memory_order_release:
        case std::memory_order_acq_rel:
        case std::memory_order_seq_cst:
            return;
    }
    throw std::invalid_argument(
        "lincheck " + std::string(operation) +
        " memory_order unknown is invalid"
    );
}

inline void throw_invalid_memory_order(
    std::string_view operation,
    std::memory_order order,
    std::string_view reason
) {
    throw std::invalid_argument(
        "lincheck " + std::string(operation) +
        " memory_order " + std::string(memory_order_name(order)) +
        " is invalid: " + std::string(reason)
    );
}

inline void validate_load_memory_order(std::memory_order order, std::string_view operation) {
    validate_known_memory_order(order, operation);
    if (order == std::memory_order_release || order == std::memory_order_acq_rel) {
        throw_invalid_memory_order(operation, order, "load must not use release or acq_rel");
    }
}

inline void validate_store_memory_order(std::memory_order order, std::string_view operation) {
    validate_known_memory_order(order, operation);
    if (
        order == std::memory_order_consume ||
        order == std::memory_order_acquire ||
        order == std::memory_order_acq_rel
    ) {
        throw_invalid_memory_order(operation, order, "store must not use consume, acquire, or acq_rel");
    }
}

inline void validate_wait_memory_order(std::memory_order order, std::string_view operation) {
    validate_known_memory_order(order, operation);
    if (order == std::memory_order_release || order == std::memory_order_acq_rel) {
        throw_invalid_memory_order(operation, order, "wait must not use release or acq_rel");
    }
}

inline bool compare_exchange_failure_allowed_by_success(std::memory_order success, std::memory_order failure) {
    switch (success) {
        case std::memory_order_relaxed:
        case std::memory_order_release:
            return failure == std::memory_order_relaxed;
        case std::memory_order_consume:
            return failure == std::memory_order_relaxed || failure == std::memory_order_consume;
        case std::memory_order_acquire:
        case std::memory_order_acq_rel:
            return failure == std::memory_order_relaxed ||
                failure == std::memory_order_consume ||
                failure == std::memory_order_acquire;
        case std::memory_order_seq_cst:
            return failure == std::memory_order_relaxed ||
                failure == std::memory_order_consume ||
                failure == std::memory_order_acquire ||
                failure == std::memory_order_seq_cst;
    }
    return false;
}

inline void validate_compare_exchange_memory_orders(
    std::memory_order success,
    std::memory_order failure,
    std::string_view operation
) {
    validate_known_memory_order(success, operation);
    validate_known_memory_order(failure, operation);
    if (failure == std::memory_order_release || failure == std::memory_order_acq_rel) {
        throw_invalid_memory_order(operation, failure, "compare_exchange failure must not use release or acq_rel");
    }
    if (!compare_exchange_failure_allowed_by_success(success, failure)) {
        throw std::invalid_argument(
            "lincheck " + std::string(operation) +
            " failure memory_order " + std::string(memory_order_name(failure)) +
            " is invalid: failure order must not be stronger than success order " +
            memory_order_name(success)
        );
    }
}

enum class ClockSourceKind {
    atomic_sequence,
    rdtsc
};

enum class MemoryModel {
    sequential_consistency,
    cxx_release_acquire,
    cxx_relaxed
};

inline const char* memory_model_name(MemoryModel model) {
    switch (model) {
        case MemoryModel::sequential_consistency: return "sequential_consistency";
        case MemoryModel::cxx_release_acquire: return "cxx_release_acquire";
        case MemoryModel::cxx_relaxed: return "cxx_relaxed";
    }
    return "unknown";
}

class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual std::uint64_t now() = 0;
    virtual std::string name() const = 0;
    virtual std::vector<std::string> warnings() const { return {}; }
};

class AtomicClockSource final : public ClockSource {
public:
    std::uint64_t now() override {
        return next_.fetch_add(1, std::memory_order_seq_cst);
    }

    std::string name() const override {
        return "atomic-sequence";
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

class RdtscClockSource final : public ClockSource {
public:
    std::uint64_t now() override {
#if defined(__x86_64__) || defined(__i386__)
        unsigned aux = 0;
        return static_cast<std::uint64_t>(__rdtscp(&aux));
#else
        return fallback_.now();
#endif
    }

    std::string name() const override {
#if defined(__x86_64__) || defined(__i386__)
        return "rdtscp";
#else
        return "atomic-sequence";
#endif
    }

    std::vector<std::string> warnings() const override {
#if defined(__x86_64__) || defined(__i386__)
        return {
            "rdtsc/rdtscp clocks can be inconsistent across sockets, NUMA nodes, virtual machines, older CPUs without invariant synchronized TSC, and aggressive power-management settings"
        };
#else
        return {
            "rdtsc/rdtscp is unavailable on this architecture; using atomic-sequence clock"
        };
#endif
    }

private:
    AtomicClockSource fallback_;
};

inline std::shared_ptr<ClockSource> make_clock_source(ClockSourceKind kind) {
    switch (kind) {
        case ClockSourceKind::atomic_sequence:
            return std::make_shared<AtomicClockSource>();
        case ClockSourceKind::rdtsc:
            return std::make_shared<RdtscClockSource>();
    }
    return std::make_shared<AtomicClockSource>();
}

class Generator {
public:
    using Function = std::function<Value(std::mt19937_64&)>;
    using Factory = std::function<Function()>;

    Generator() = default;

    template <typename Fn>
        requires (
            !std::is_same_v<std::decay_t<Fn>, Generator> &&
            !std::is_same_v<std::decay_t<Fn>, Function> &&
            !std::is_same_v<std::decay_t<Fn>, Factory> &&
            std::is_invocable_r_v<Value, Fn&, std::mt19937_64&>
        )
    Generator(Fn fn)
        : Generator(Function(std::move(fn))) {}

    Generator(Function fn)
        : factory_([initial = std::move(fn)] {
            return initial;
        }) {
        reset();
    }

    explicit Generator(Factory factory)
        : factory_(std::move(factory)) {
        reset();
    }

    Value operator()(std::mt19937_64& rng) {
        if (!current_) reset();
        return current_(rng);
    }

    void reset() {
        if (!factory_) {
            current_ = {};
            return;
        }
        current_ = factory_();
    }

private:
    Factory factory_;
    Function current_;
};

template <typename T>
Generator constant(T value) {
    return [value = Value(value)](std::mt19937_64&) { return value; };
}

inline Generator booleans() {
    return [](std::mt19937_64& rng) -> Value {
        std::uniform_int_distribution<int> dist(0, 1);
        return Value(dist(rng) != 0);
    };
}

template <typename T>
Generator range(T min, T max) {
    if (min > max) {
        throw std::invalid_argument("range generator minimum must be <= maximum");
    }
    return [min, max](std::mt19937_64& rng) -> Value {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            std::uniform_int_distribution<long long> dist(static_cast<long long>(min), static_cast<long long>(max));
            return Value(static_cast<T>(dist(rng)));
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            std::uniform_int_distribution<unsigned long long> dist(static_cast<unsigned long long>(min), static_cast<unsigned long long>(max));
            return Value(static_cast<T>(dist(rng)));
        } else {
            std::uniform_real_distribution<double> dist(static_cast<double>(min), static_cast<double>(max));
            return Value(static_cast<T>(dist(rng)));
        }
    };
}

template <typename T>
Generator values(std::vector<T> domain) {
    if (domain.empty()) {
        throw std::invalid_argument("values generator requires at least one value");
    }
    return [domain = std::move(domain)](std::mt19937_64& rng) -> Value {
        std::uniform_int_distribution<std::size_t> dist(0, domain.size() - 1);
        return Value(domain[dist(rng)]);
    };
}

template <typename T>
Generator values(std::initializer_list<T> domain) {
    return values(std::vector<T>(domain));
}

inline Generator strings(std::vector<std::string> domain) {
    return values(std::move(domain));
}

inline Generator strings(std::initializer_list<std::string> domain) {
    return strings(std::vector<std::string>(domain));
}

inline Generator strings(
    std::size_t min_length,
    std::size_t max_length,
    std::string alphabet = "abcdefghijklmnopqrstuvwxyz"
) {
    if (min_length > max_length) {
        throw std::invalid_argument("string generator minimum length must be <= maximum length");
    }
    if (alphabet.empty() && max_length > 0) {
        throw std::invalid_argument("string generator alphabet must not be empty for non-empty strings");
    }

    return [min_length, max_length, alphabet = std::move(alphabet)](std::mt19937_64& rng) -> Value {
        std::uniform_int_distribution<std::size_t> length_dist(min_length, max_length);
        const auto length = length_dist(rng);
        std::string result;
        result.reserve(length);
        if (length == 0) return Value(result);

        std::uniform_int_distribution<std::size_t> char_dist(0, alphabet.size() - 1);
        for (std::size_t i = 0; i < length; ++i) {
            result.push_back(alphabet[char_dist(rng)]);
        }
        return Value(std::move(result));
    };
}

template <typename T>
    requires (
        std::same_as<std::decay_t<T>, bool> ||
        std::is_enum_v<std::decay_t<T>> ||
        (std::integral<std::decay_t<T>> && !std::same_as<std::decay_t<T>, bool>) ||
        std::floating_point<std::decay_t<T>> ||
        std::same_as<std::decay_t<T>, std::string>
    )
Generator gen() {
    using Decayed = std::decay_t<T>;
    if constexpr (std::same_as<Decayed, bool>) {
        return booleans();
    } else if constexpr (std::is_enum_v<Decayed>) {
        using Underlying = std::underlying_type_t<Decayed>;
        return gen<Underlying>();
    } else if constexpr (std::integral<Decayed> && std::is_signed_v<Decayed>) {
        return range<Decayed>(static_cast<Decayed>(-2), static_cast<Decayed>(2));
    } else if constexpr (std::integral<Decayed> && std::is_unsigned_v<Decayed>) {
        return range<Decayed>(static_cast<Decayed>(0), static_cast<Decayed>(4));
    } else if constexpr (std::floating_point<Decayed>) {
        return range<Decayed>(static_cast<Decayed>(-1), static_cast<Decayed>(1));
    } else {
        return strings(0, 4);
    }
}

template <typename Fn>
Generator custom(Fn fn) {
    return [fn = std::move(fn)](std::mt19937_64& rng) mutable -> Value {
        return to_value(fn(rng));
    };
}

struct Actor {
    std::size_t operation_index = 0;
    std::string name;
    std::string group;
    bool non_parallel = false;
    bool one_shot = false;
    bool exception_results = false;
    std::vector<Value> arguments;

    std::string to_string() const {
        std::ostringstream out;
        out << name << "(";
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i != 0) out << ", ";
            out << arguments[i].to_string();
        }
        out << ")";
        if (!group.empty() || non_parallel || one_shot || exception_results) {
            out << " [";
            bool first = true;
            auto append = [&](const std::string& value) {
                if (!first) out << " ";
                out << value;
                first = false;
            };
            if (!group.empty()) append("group=" + group);
            if (non_parallel) append("non_parallel");
            if (one_shot) append("one_shot");
            if (exception_results) append("exceptions_as_results");
            out << "]";
        }
        return out.str();
    }
};

struct OperationOptions {
    std::string group;
    std::string independence_group;
    bool non_parallel = false;
    bool one_shot = false;
    bool exception_results = false;

    OperationOptions& group_name(std::string value) {
        group = std::move(value);
        return *this;
    }

    OperationOptions& non_parallel_group(std::string value) {
        group = std::move(value);
        non_parallel = true;
        return *this;
    }

    OperationOptions& independent_group(std::string value) {
        independence_group = std::move(value);
        return *this;
    }

    OperationOptions& one_shot_operation(bool value = true) {
        one_shot = value;
        return *this;
    }

    OperationOptions& exceptions_as_results(bool value = true) {
        exception_results = value;
        return *this;
    }
};

inline OperationOptions operation_group(std::string group) {
    OperationOptions options;
    options.group = std::move(group);
    return options;
}

inline OperationOptions non_parallel_group(std::string group) {
    OperationOptions options;
    options.group = std::move(group);
    options.non_parallel = true;
    return options;
}

inline OperationOptions independent_operation_group(std::string group) {
    OperationOptions options;
    options.independence_group = std::move(group);
    return options;
}

inline OperationOptions one_shot() {
    OperationOptions options;
    options.one_shot = true;
    return options;
}

inline OperationOptions exceptions_as_results() {
    OperationOptions options;
    options.exception_results = true;
    return options;
}

struct OperationInterval {
    int thread_id = -1;
    std::size_t actor_index = 0;
    std::uint64_t invocation_clock = 0;
    std::uint64_t response_clock = 0;
    Value result;
};

struct OperationContext {
    int thread_id = -1;
    std::size_t actor_index = 0;
    std::size_t operation_index = 0;
    std::string name;
    std::string group;
    std::string independence_group;
    bool non_parallel = false;
    bool one_shot = false;
    bool exception_results = false;
    std::string actor_label;
};

struct ScheduleDecision {
    std::size_t switch_position = 0;
    int thread_id = -1;
    std::string location;
    std::vector<int> runnable_threads;
    int chosen_thread = -1;
    std::vector<OperationContext> runnable_operations;
};

inline std::string operation_context_label(const OperationContext& operation) {
    std::ostringstream out;
    out << operation.name << "@" << operation.actor_index << "#" << operation.operation_index;
    if (!operation.group.empty()) {
        out << " group=" << operation.group;
    }
    if (!operation.independence_group.empty()) {
        out << " independent=" << operation.independence_group;
    }
    if (operation.non_parallel) {
        out << " non_parallel";
    }
    if (operation.one_shot) {
        out << " one_shot";
    }
    if (operation.exception_results) {
        out << " exceptions_as_results";
    }
    return out.str();
}

inline bool same_operation_context_identity(const OperationContext& left, const OperationContext& right) {
    return left.thread_id == right.thread_id &&
        left.actor_index == right.actor_index &&
        left.operation_index == right.operation_index &&
        left.name == right.name &&
        left.group == right.group &&
        left.independence_group == right.independence_group &&
        left.non_parallel == right.non_parallel &&
        left.one_shot == right.one_shot &&
        left.exception_results == right.exception_results &&
        left.actor_label == right.actor_label;
}

inline void append_operation_context_label(std::ostream& out, const std::optional<OperationContext>& operation) {
    if (operation) {
        out << " operation=" << operation_context_label(*operation);
    }
}

struct ExecutionScenario {
    std::vector<Actor> init;
    std::vector<std::vector<Actor>> parallel;
    std::vector<Actor> post;

    bool valid() const {
        return std::any_of(parallel.begin(), parallel.end(), [](const auto& actors) {
            return !actors.empty();
        });
    }

    std::string to_string() const {
        std::ostringstream out;
        out << "init:";
        for (const auto& actor : init) out << " " << actor.to_string();
        out << "\nparallel:\n";
        for (std::size_t t = 0; t < parallel.size(); ++t) {
            out << "  thread " << t << ":";
            for (const auto& actor : parallel[t]) out << " " << actor.to_string();
            out << "\n";
        }
        out << "post:";
        for (const auto& actor : post) out << " " << actor.to_string();
        return out.str();
    }
};

struct Operation {
    std::string name;
    OperationOptions options;
    std::size_t argument_count = 0;
    std::vector<Generator> generators;
    std::function<Value(void*, const std::vector<Value>&)> run_concurrent;
    std::function<Value(void*, const std::vector<Value>&)> run_sequential;
};

struct TestSpec {
    std::vector<Operation> operations;
    std::function<std::shared_ptr<void>()> make_concurrent;
    std::function<std::shared_ptr<void>()> make_sequential;
    std::function<std::shared_ptr<void>(const std::shared_ptr<void>&)> clone_sequential;
    std::function<Value(void*)> sequential_state_key;
    std::function<std::string(void*)> describe_concurrent;
    std::function<std::string(void*)> validate_concurrent;
};

namespace detail {

template <typename...>
inline constexpr bool always_false_v = false;

template <typename Arg>
std::decay_t<Arg> argument_cast(const Value& value) {
    return value_cast<std::decay_t<Arg>>(value);
}

template <typename Obj, typename R, typename... Args, std::size_t... I>
Value invoke_member(Obj& obj, R (Obj::*method)(Args...), const std::vector<Value>& args, std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) {
        (obj.*method)(argument_cast<Args>(args[I])...);
        return {};
    } else {
        return to_value((obj.*method)(argument_cast<Args>(args[I])...));
    }
}

template <typename Obj, typename R, typename... Args, std::size_t... I>
Value invoke_const_member(const Obj& obj, R (Obj::*method)(Args...) const, const std::vector<Value>& args, std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) {
        (obj.*method)(argument_cast<Args>(args[I])...);
        return {};
    } else {
        return to_value((obj.*method)(argument_cast<Args>(args[I])...));
    }
}

template <typename Obj, typename Fn, typename... Args, std::size_t... I>
Value invoke_callable_with_arguments(void* raw, Fn& fn, const std::vector<Value>& args, std::index_sequence<I...>) {
    if constexpr (std::is_invocable_v<Fn&, Obj&, Args...>) {
        Obj& object = *static_cast<Obj*>(raw);
        using Result = std::invoke_result_t<Fn&, Obj&, Args...>;
        if constexpr (std::is_void_v<Result>) {
            std::invoke(fn, object, argument_cast<Args>(args[I])...);
            return {};
        } else {
            return to_value(std::invoke(fn, object, argument_cast<Args>(args[I])...));
        }
    } else if constexpr (std::is_invocable_v<Fn&, Obj*, Args...>) {
        Obj* object = static_cast<Obj*>(raw);
        using Result = std::invoke_result_t<Fn&, Obj*, Args...>;
        if constexpr (std::is_void_v<Result>) {
            std::invoke(fn, object, argument_cast<Args>(args[I])...);
            return {};
        } else {
            return to_value(std::invoke(fn, object, argument_cast<Args>(args[I])...));
        }
    } else {
        static_assert(always_false_v<Fn, Obj, Args...>, "operation callable must accept Obj& or Obj* followed by the registered argument types");
    }
}

template <typename Obj, typename Method>
struct MemberWrapper;

template <typename Method>
struct MemberFunctionArity;

template <typename Obj, typename R, typename... Args>
struct MemberFunctionArity<R (Obj::*)(Args...)> : std::integral_constant<std::size_t, sizeof...(Args)> {};

template <typename Obj, typename R, typename... Args>
struct MemberFunctionArity<R (Obj::*)(Args...) const> : std::integral_constant<std::size_t, sizeof...(Args)> {};

template <typename Method>
inline constexpr std::size_t member_function_arity_v = MemberFunctionArity<Method>::value;

template <typename Obj, typename R, typename... Args>
struct MemberWrapper<Obj, R (Obj::*)(Args...)> {
    using Method = R (Obj::*)(Args...);

    static Value call(void* raw, Method method, const std::vector<Value>& args) {
        if (args.size() != sizeof...(Args)) {
            throw std::invalid_argument("operation argument count mismatch");
        }
        return invoke_member(*static_cast<Obj*>(raw), method, args, std::index_sequence_for<Args...>{});
    }
};

template <typename Obj, typename R, typename... Args>
struct MemberWrapper<Obj, R (Obj::*)(Args...) const> {
    using Method = R (Obj::*)(Args...) const;

    static Value call(void* raw, Method method, const std::vector<Value>& args) {
        if (args.size() != sizeof...(Args)) {
            throw std::invalid_argument("operation argument count mismatch");
        }
        return invoke_const_member(*static_cast<const Obj*>(raw), method, args, std::index_sequence_for<Args...>{});
    }
};

template <typename Obj, typename Fn, typename... Args>
struct CallableWrapper {
    static Value call(void* raw, Fn& fn, const std::vector<Value>& args) {
        if (args.size() != sizeof...(Args)) {
            throw std::invalid_argument("operation argument count mismatch");
        }
        return invoke_callable_with_arguments<Obj, Fn, Args...>(raw, fn, args, std::index_sequence_for<Args...>{});
    }
};

template <typename T, typename Result>
std::shared_ptr<void> erase_factory_result(Result&& result) {
    using Decayed = std::decay_t<Result>;
    if constexpr (std::is_convertible_v<Decayed, std::shared_ptr<T>>) {
        return std::forward<Result>(result);
    } else if constexpr (std::is_convertible_v<Decayed, std::unique_ptr<T>>) {
        return std::shared_ptr<T>(std::forward<Result>(result));
    } else {
        return std::make_shared<T>(std::forward<Result>(result));
    }
}

} // namespace detail

template <typename Concurrent, typename Sequential>
class TestBuilder {
public:
    TestBuilder() {
        if constexpr (std::default_initializable<Concurrent>) {
            spec_.make_concurrent = [] { return std::make_shared<Concurrent>(); };
        }
        if constexpr (std::default_initializable<Sequential>) {
            spec_.make_sequential = [] { return std::make_shared<Sequential>(); };
        }
        if constexpr (std::copy_constructible<Sequential>) {
            spec_.clone_sequential = [](const std::shared_ptr<void>& model) {
                return std::make_shared<Sequential>(*std::static_pointer_cast<Sequential>(model));
            };
        }
    }

    template <typename Fn>
    TestBuilder& concurrent_factory(Fn fn) {
        spec_.make_concurrent = [fn = std::move(fn)]() mutable {
            return detail::erase_factory_result<Concurrent>(fn());
        };
        return *this;
    }

    template <typename Fn>
    TestBuilder& sequential_factory(Fn fn) {
        spec_.make_sequential = [fn = std::move(fn)]() mutable {
            return detail::erase_factory_result<Sequential>(fn());
        };
        return *this;
    }

    template <typename Fn>
    TestBuilder& sequential_cloner(Fn fn) {
        spec_.clone_sequential = [fn = std::move(fn)](const std::shared_ptr<void>& model) mutable {
            const auto typed = std::static_pointer_cast<Sequential>(model);
            return detail::erase_factory_result<Sequential>(fn(*typed));
        };
        return *this;
    }

    template <typename ConcurrentMethod, typename SequentialMethod, typename... Generators>
    TestBuilder& operation(std::string name, ConcurrentMethod concurrent, SequentialMethod sequential, Generators... generators) {
        return operation_with_options(
            std::move(name),
            concurrent,
            sequential,
            OperationOptions{},
            std::move(generators)...
        );
    }

    template <typename ConcurrentMethod, typename SequentialMethod, typename... Generators>
    TestBuilder& operation_with_options(
        std::string name,
        ConcurrentMethod concurrent,
        SequentialMethod sequential,
        OperationOptions options,
        Generators... generators
    ) {
        constexpr auto concurrent_arity = detail::member_function_arity_v<ConcurrentMethod>;
        constexpr auto sequential_arity = detail::member_function_arity_v<SequentialMethod>;
        static_assert(
            concurrent_arity == sequential_arity,
            "operation concurrent and sequential member functions must have the same argument count"
        );
        static_assert(
            sizeof...(Generators) == concurrent_arity,
            "operation generator count must match member function argument count"
        );

        Operation op;
        op.name = std::move(name);
        op.options = std::move(options);
        op.argument_count = concurrent_arity;
        op.generators = {Generator(generators)...};
        op.run_concurrent = [concurrent](void* object, const std::vector<Value>& args) {
            return detail::MemberWrapper<Concurrent, ConcurrentMethod>::call(object, concurrent, args);
        };
        op.run_sequential = [sequential](void* object, const std::vector<Value>& args) {
            return detail::MemberWrapper<Sequential, SequentialMethod>::call(object, sequential, args);
        };
        spec_.operations.push_back(std::move(op));
        return *this;
    }

    template <typename... Args, typename ConcurrentFn, typename SequentialFn, typename... Generators>
    TestBuilder& operation_callable(
        std::string name,
        ConcurrentFn concurrent,
        SequentialFn sequential,
        Generators... generators
    ) {
        return operation_callable_with_options<Args...>(
            std::move(name),
            std::move(concurrent),
            std::move(sequential),
            OperationOptions{},
            std::move(generators)...
        );
    }

    template <typename... Args, typename ConcurrentFn, typename SequentialFn, typename... Generators>
    TestBuilder& operation_callable_with_options(
        std::string name,
        ConcurrentFn concurrent,
        SequentialFn sequential,
        OperationOptions options,
        Generators... generators
    ) {
        static_assert(
            sizeof...(Args) == sizeof...(Generators),
            "operation_callable argument type count must match generator count"
        );

        using ConcurrentCallable = std::decay_t<ConcurrentFn>;
        using SequentialCallable = std::decay_t<SequentialFn>;

        Operation op;
        op.name = std::move(name);
        op.options = std::move(options);
        op.argument_count = sizeof...(Args);
        op.generators = {Generator(generators)...};
        op.run_concurrent = [concurrent = ConcurrentCallable(std::move(concurrent))](void* object, const std::vector<Value>& args) mutable {
            return detail::CallableWrapper<Concurrent, ConcurrentCallable, Args...>::call(object, concurrent, args);
        };
        op.run_sequential = [sequential = SequentialCallable(std::move(sequential))](void* object, const std::vector<Value>& args) mutable {
            return detail::CallableWrapper<Sequential, SequentialCallable, Args...>::call(object, sequential, args);
        };
        spec_.operations.push_back(std::move(op));
        return *this;
    }

    template <typename Fn>
    TestBuilder& state_representation(Fn fn) {
        spec_.describe_concurrent = [fn = std::move(fn)](void* object) mutable {
            return fn(*static_cast<Concurrent*>(object));
        };
        return *this;
    }

    template <typename Fn>
    TestBuilder& sequential_state(Fn fn) {
        spec_.sequential_state_key = [fn = std::move(fn)](void* object) mutable {
            return to_value(fn(*static_cast<Sequential*>(object)));
        };
        return *this;
    }

    template <typename Fn>
    TestBuilder& validation(Fn fn) {
        spec_.validate_concurrent = [fn = std::move(fn)](void* object) mutable -> std::string {
            auto& target = *static_cast<Concurrent*>(object);
            using Result = std::invoke_result_t<Fn&, Concurrent&>;
            if constexpr (std::is_void_v<Result>) {
                fn(target);
                return {};
            } else if constexpr (std::is_same_v<std::decay_t<Result>, bool>) {
                return fn(target) ? std::string{} : std::string{"validation failed"};
            } else if constexpr (std::is_convertible_v<Result, std::string>) {
                return std::string(fn(target));
            } else {
                return to_value(fn(target)).to_string();
            }
        };
        return *this;
    }

    TestSpec build() const {
        if (!spec_.make_concurrent) {
            throw std::invalid_argument("lincheck concurrent object type is not default-initializable; call concurrent_factory(...)");
        }
        if (!spec_.make_sequential) {
            throw std::invalid_argument("lincheck sequential model type is not default-initializable; call sequential_factory(...)");
        }
        if (!spec_.clone_sequential) {
            throw std::invalid_argument("lincheck sequential model type is not copy-constructible; call sequential_cloner(...)");
        }
        return spec_;
    }
    operator TestSpec() const { return build(); }

private:
    TestSpec spec_;
};

template <typename Concurrent, typename Sequential>
TestBuilder<Concurrent, Sequential> test() {
    return {};
}

struct ExecutionResult {
    std::vector<Value> init_results;
    std::vector<std::vector<Value>> parallel_results;
    std::vector<Value> post_results;
    std::vector<OperationInterval> init_intervals;
    std::vector<std::vector<OperationInterval>> parallel_intervals;
    std::vector<OperationInterval> post_intervals;
};

struct TraceEventRecord {
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    std::uint64_t transaction_id = 0;
    int transaction_depth = 0;
    std::string kind;
    std::string description;
};

namespace detail {

struct TraceTransactionMetadata {
    std::uint64_t transaction_id = 0;
    int transaction_depth = 0;
};

inline thread_local std::optional<TraceTransactionMetadata> active_trace_transaction_metadata;

class ScopedTraceTransactionMetadata {
public:
    ScopedTraceTransactionMetadata(std::uint64_t transaction_id, int transaction_depth)
        : previous_(active_trace_transaction_metadata) {
        if (transaction_id != 0 || transaction_depth != 0) {
            active_trace_transaction_metadata = TraceTransactionMetadata{
                .transaction_id = transaction_id,
                .transaction_depth = transaction_depth
            };
        } else {
            active_trace_transaction_metadata.reset();
        }
    }

    ~ScopedTraceTransactionMetadata() {
        active_trace_transaction_metadata = previous_;
    }

    ScopedTraceTransactionMetadata(const ScopedTraceTransactionMetadata&) = delete;
    ScopedTraceTransactionMetadata& operator=(const ScopedTraceTransactionMetadata&) = delete;

private:
    std::optional<TraceTransactionMetadata> previous_;
};

inline std::string trace_event_kind(std::string_view description) {
    const auto first = description.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = description.find_first_of(" \t\r\n", first);
    return std::string(description.substr(first, last == std::string_view::npos ? description.size() - first : last - first));
}

inline TraceEventRecord make_trace_event_record(
    std::size_t sequence,
    int thread_id,
    std::string kind,
    std::string description,
    std::size_t event_index = 0,
    const OperationContext* operation = nullptr
) {
    TraceEventRecord record;
    record.sequence = sequence;
    record.event_index = event_index;
    record.thread_id = thread_id;
    if (operation != nullptr) {
        record.operation = *operation;
    }
    if (active_trace_transaction_metadata) {
        record.transaction_id = active_trace_transaction_metadata->transaction_id;
        record.transaction_depth = active_trace_transaction_metadata->transaction_depth;
    }
    record.kind = std::move(kind);
    record.description = std::move(description);
    if (record.kind.empty()) {
        record.kind = trace_event_kind(record.description);
    }
    return record;
}

} // namespace detail

inline std::string to_string(const TraceEventRecord& record) {
    std::ostringstream out;
    out << "#" << record.sequence
        << " event=" << record.event_index
        << " thread=" << record.thread_id;
    if (!record.kind.empty()) {
        out << " kind=" << record.kind;
    }
    append_operation_context_label(out, record.operation);
    if (record.transaction_id != 0 &&
        record.description.find("tx_id=") == std::string::npos) {
        out << " tx_id=" << record.transaction_id;
    }
    if (record.transaction_depth != 0 &&
        record.description.find("tx_depth=") == std::string::npos) {
        out << " tx_depth=" << record.transaction_depth;
    }
    if (!record.description.empty()) {
        out << " " << record.description;
    }
    return out.str();
}

enum class MemoryEventKind {
    atomic_load,
    atomic_store,
    atomic_exchange,
    atomic_fetch_add,
    atomic_fetch_sub,
    atomic_fetch_and,
    atomic_fetch_or,
    atomic_fetch_xor,
    atomic_compare_exchange_strong,
    atomic_compare_exchange_weak,
    atomic_wait,
    atomic_notify_one,
    atomic_notify_all,
    atomic_thread_fence,
    atomic_signal_fence
};

struct MemoryEvent {
    MemoryEventKind kind = MemoryEventKind::atomic_load;
    const void* object = nullptr;
    SourceLocation source;
    bool has_source = false;
    std::memory_order success_order = std::memory_order_seq_cst;
    std::memory_order failure_order = std::memory_order_seq_cst;
    bool has_failure_order = false;
    Value operand;
    bool has_operand = false;
    Value expected;
    bool has_expected = false;
    Value observed;
    bool has_observed = false;
    bool success = false;
    bool has_success = false;
};

inline const char* memory_event_kind_name(MemoryEventKind kind) {
    switch (kind) {
        case MemoryEventKind::atomic_load: return "atomic.load";
        case MemoryEventKind::atomic_store: return "atomic.store";
        case MemoryEventKind::atomic_exchange: return "atomic.exchange";
        case MemoryEventKind::atomic_fetch_add: return "atomic.fetch_add";
        case MemoryEventKind::atomic_fetch_sub: return "atomic.fetch_sub";
        case MemoryEventKind::atomic_fetch_and: return "atomic.fetch_and";
        case MemoryEventKind::atomic_fetch_or: return "atomic.fetch_or";
        case MemoryEventKind::atomic_fetch_xor: return "atomic.fetch_xor";
        case MemoryEventKind::atomic_compare_exchange_strong: return "atomic.compare_exchange_strong";
        case MemoryEventKind::atomic_compare_exchange_weak: return "atomic.compare_exchange_weak";
        case MemoryEventKind::atomic_wait: return "atomic.wait";
        case MemoryEventKind::atomic_notify_one: return "atomic.notify_one";
        case MemoryEventKind::atomic_notify_all: return "atomic.notify_all";
        case MemoryEventKind::atomic_thread_fence: return "atomic_thread_fence";
        case MemoryEventKind::atomic_signal_fence: return "atomic_signal_fence";
    }
    return "unknown";
}

inline std::string to_string(const MemoryEvent& event) {
    std::ostringstream out;
    out << memory_event_kind_name(event.kind)
        << " order=" << memory_order_name(event.success_order);
    if (event.has_failure_order) {
        out << " failure_order=" << memory_order_name(event.failure_order);
    }
    if (event.object != nullptr) {
        out << " object=" << stable_object_id(event.object);
    }
    if (event.has_source) {
        out << " source=" << event.source.to_string()
            << " location=" << stable_location_id(event.source);
    }
    if (event.has_operand) {
        out << " operand=" << event.operand.to_string();
    }
    if (event.has_expected) {
        out << " expected=" << event.expected.to_string();
    }
    if (event.has_observed) {
        out << " observed=" << event.observed.to_string();
    }
    if (event.has_success) {
        out << " success=" << (event.success ? "true" : "false");
    }
    return out.str();
}

struct MemoryEventRecord {
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    MemoryEvent event;
    std::string object_id;
    SourceLocation source;
    bool has_source = false;
    std::string location_id;
};

namespace detail {

inline MemoryEventRecord make_memory_event_record(
    std::size_t sequence,
    int thread_id,
    const MemoryEvent& event,
    std::size_t event_index = 0,
    const OperationContext* operation = nullptr
) {
    MemoryEventRecord record;
    record.sequence = sequence;
    record.event_index = event_index;
    record.thread_id = thread_id;
    if (operation != nullptr) {
        record.operation = *operation;
    }
    record.event = event;
    if (event.object != nullptr) {
        record.object_id = stable_object_id(event.object);
    }
    if (event.has_source) {
        record.source = event.source;
        record.has_source = true;
        record.location_id = stable_location_id(event.source);
    }
    return record;
}

} // namespace detail

inline std::string to_string(const MemoryEventRecord& record) {
    std::ostringstream out;
    out << "#" << record.sequence
        << " event=" << record.event_index
        << " thread=" << record.thread_id
        << " " << memory_event_kind_name(record.event.kind)
        << " order=" << memory_order_name(record.event.success_order);
    append_operation_context_label(out, record.operation);
    if (record.event.has_failure_order) {
        out << " failure_order=" << memory_order_name(record.event.failure_order);
    }
    if (!record.object_id.empty()) {
        out << " object=" << record.object_id;
    } else if (record.event.object != nullptr) {
        out << " object=" << stable_object_id(record.event.object);
    }
    if (record.has_source) {
        out << " source=" << record.source.to_string();
        if (!record.location_id.empty()) {
            out << " location=" << record.location_id;
        } else {
            out << " location=" << stable_location_id(record.source);
        }
    } else if (record.event.has_source) {
        out << " source=" << record.event.source.to_string()
            << " location=" << stable_location_id(record.event.source);
    }
    if (record.event.has_operand) {
        out << " operand=" << record.event.operand.to_string();
    }
    if (record.event.has_expected) {
        out << " expected=" << record.event.expected.to_string();
    }
    if (record.event.has_observed) {
        out << " observed=" << record.event.observed.to_string();
    }
    if (record.event.has_success) {
        out << " success=" << (record.event.success ? "true" : "false");
    }
    return out.str();
}

struct StmEventRecord {
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    std::string kind;
    std::string description;
    std::string address_id;
    bool read_only = false;
    bool has_read_only = false;
    std::uint64_t lock_slot = 0;
    bool has_lock_slot = false;
    std::uint64_t version = 0;
    bool has_version = false;
    std::uint64_t clock = 0;
    bool has_clock = false;
    bool success = false;
    bool has_success = false;
    int attempt = 0;
    std::string reason;
    std::uint64_t transaction_id = 0;
    int transaction_depth = 0;
};

inline std::string to_string(const StmEventRecord& record) {
    std::ostringstream out;
    out << "#" << record.sequence
        << " event=" << record.event_index
        << " thread=" << record.thread_id
        << " " << record.description;
    append_operation_context_label(out, record.operation);
    if (!record.address_id.empty() &&
        record.description.find("address=" + record.address_id) == std::string::npos) {
        out << " address=" << record.address_id;
    }
    if (record.transaction_id != 0 &&
        record.description.find("tx_id=") == std::string::npos) {
        out << " tx_id=" << record.transaction_id;
    }
    if (record.transaction_depth != 0 &&
        record.description.find("tx_depth=") == std::string::npos) {
        out << " tx_depth=" << record.transaction_depth;
    }
    return out.str();
}

enum class SourceAccessKind {
    read,
    write
};

struct SourceAccessEvent {
    SourceAccessKind kind = SourceAccessKind::read;
    const void* object = nullptr;
    SourceLocation source;
    Value value;
    bool has_value = false;
};

inline const char* source_access_kind_name(SourceAccessKind kind) {
    switch (kind) {
        case SourceAccessKind::read: return "source.read";
        case SourceAccessKind::write: return "source.write";
    }
    return "source.unknown";
}

inline std::string to_string(const SourceAccessEvent& event) {
    std::ostringstream out;
    out << source_access_kind_name(event.kind);
    if (event.object != nullptr) {
        out << " object=" << stable_object_id(event.object);
    }
    out << " source=" << event.source.to_string()
        << " location=" << stable_location_id(event.source);
    if (event.has_value) {
        out << " value=" << event.value.to_string();
    }
    return out.str();
}

struct SourceAccessEventRecord {
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    SourceAccessEvent event;
    std::string object_id;
    SourceLocation source;
    std::string location_id;
};

namespace detail {

inline SourceAccessEventRecord make_source_access_event_record(
    std::size_t sequence,
    int thread_id,
    const SourceAccessEvent& event,
    std::size_t event_index = 0,
    const OperationContext* operation = nullptr
) {
    SourceAccessEventRecord record;
    record.sequence = sequence;
    record.event_index = event_index;
    record.thread_id = thread_id;
    if (operation != nullptr) {
        record.operation = *operation;
    }
    record.event = event;
    if (event.object != nullptr) {
        record.object_id = stable_object_id(event.object);
    }
    record.source = event.source;
    record.location_id = stable_location_id(event.source);
    return record;
}

} // namespace detail

inline std::string to_string(const SourceAccessEventRecord& record) {
    std::ostringstream out;
    out << "#" << record.sequence
        << " event=" << record.event_index
        << " thread=" << record.thread_id
        << " " << source_access_kind_name(record.event.kind);
    append_operation_context_label(out, record.operation);
    if (!record.object_id.empty()) {
        out << " object=" << record.object_id;
    } else if (record.event.object != nullptr) {
        out << " object=" << stable_object_id(record.event.object);
    }
    out << " source=" << record.source.to_string();
    if (!record.location_id.empty()) {
        out << " location=" << record.location_id;
    } else {
        out << " location=" << stable_location_id(record.source);
    }
    if (record.event.has_value) {
        out << " value=" << record.event.value.to_string();
    }
    return out.str();
}

enum class SynchronizationEventKind {
    mutex_lock_attempt,
    mutex_lock_acquired,
    mutex_unlock,
    mutex_try_lock,
    condition_wait,
    condition_wake,
    condition_notify_one,
    condition_notify_all,
    atomic_wait,
    atomic_wake,
    atomic_notify_one,
    atomic_notify_all,
    parker_park,
    parker_unpark,
    semaphore_acquire,
    semaphore_release,
    semaphore_try_acquire,
    latch_count_down,
    latch_wait,
    latch_wake,
    latch_try_wait,
    barrier_arrive,
    barrier_wait,
    barrier_wake,
    barrier_drop,
    barrier_phase_complete
};

inline const char* synchronization_event_kind_name(SynchronizationEventKind kind) {
    switch (kind) {
        case SynchronizationEventKind::mutex_lock_attempt: return "sync.mutex.lock_attempt";
        case SynchronizationEventKind::mutex_lock_acquired: return "sync.mutex.lock_acquired";
        case SynchronizationEventKind::mutex_unlock: return "sync.mutex.unlock";
        case SynchronizationEventKind::mutex_try_lock: return "sync.mutex.try_lock";
        case SynchronizationEventKind::condition_wait: return "sync.condition_variable.wait";
        case SynchronizationEventKind::condition_wake: return "sync.condition_variable.wake";
        case SynchronizationEventKind::condition_notify_one: return "sync.condition_variable.notify_one";
        case SynchronizationEventKind::condition_notify_all: return "sync.condition_variable.notify_all";
        case SynchronizationEventKind::atomic_wait: return "sync.atomic.wait";
        case SynchronizationEventKind::atomic_wake: return "sync.atomic.wake";
        case SynchronizationEventKind::atomic_notify_one: return "sync.atomic.notify_one";
        case SynchronizationEventKind::atomic_notify_all: return "sync.atomic.notify_all";
        case SynchronizationEventKind::parker_park: return "sync.parker.park";
        case SynchronizationEventKind::parker_unpark: return "sync.parker.unpark";
        case SynchronizationEventKind::semaphore_acquire: return "sync.semaphore.acquire";
        case SynchronizationEventKind::semaphore_release: return "sync.semaphore.release";
        case SynchronizationEventKind::semaphore_try_acquire: return "sync.semaphore.try_acquire";
        case SynchronizationEventKind::latch_count_down: return "sync.latch.count_down";
        case SynchronizationEventKind::latch_wait: return "sync.latch.wait";
        case SynchronizationEventKind::latch_wake: return "sync.latch.wake";
        case SynchronizationEventKind::latch_try_wait: return "sync.latch.try_wait";
        case SynchronizationEventKind::barrier_arrive: return "sync.barrier.arrive";
        case SynchronizationEventKind::barrier_wait: return "sync.barrier.wait";
        case SynchronizationEventKind::barrier_wake: return "sync.barrier.wake";
        case SynchronizationEventKind::barrier_drop: return "sync.barrier.drop";
        case SynchronizationEventKind::barrier_phase_complete: return "sync.barrier.phase_complete";
    }
    return "sync.unknown";
}

struct SynchronizationEvent {
    SynchronizationEventKind kind = SynchronizationEventKind::mutex_lock_attempt;
    const void* object = nullptr;
    const void* related_object = nullptr;
    std::string detail;
    bool success = false;
    bool has_success = false;
};

inline std::string to_string(const SynchronizationEvent& event) {
    std::ostringstream out;
    out << synchronization_event_kind_name(event.kind);
    if (event.object != nullptr) {
        out << " object=" << stable_object_id(event.object);
    }
    if (event.related_object != nullptr) {
        out << " related=" << stable_object_id(event.related_object);
    }
    if (event.has_success) {
        out << " success=" << (event.success ? "true" : "false");
    }
    if (!event.detail.empty()) {
        out << " detail=" << event.detail;
    }
    return out.str();
}

struct SynchronizationEventRecord {
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    SynchronizationEvent event;
    std::string object_id;
    std::string related_object_id;
};

namespace detail {

inline SynchronizationEventRecord make_synchronization_event_record(
    std::size_t sequence,
    int thread_id,
    const SynchronizationEvent& event,
    std::size_t event_index = 0,
    const OperationContext* operation = nullptr
) {
    SynchronizationEventRecord record;
    record.sequence = sequence;
    record.event_index = event_index;
    record.thread_id = thread_id;
    if (operation != nullptr) {
        record.operation = *operation;
    }
    record.event = event;
    if (event.object != nullptr) {
        record.object_id = stable_object_id(event.object);
    }
    if (event.related_object != nullptr) {
        record.related_object_id = stable_object_id(event.related_object);
    }
    return record;
}

} // namespace detail

inline std::string to_string(const SynchronizationEventRecord& record) {
    std::ostringstream out;
    out << "#" << record.sequence
        << " event=" << record.event_index
        << " thread=" << record.thread_id
        << " " << synchronization_event_kind_name(record.event.kind);
    append_operation_context_label(out, record.operation);
    if (!record.object_id.empty()) {
        out << " object=" << record.object_id;
    } else if (record.event.object != nullptr) {
        out << " object=" << stable_object_id(record.event.object);
    }
    if (!record.related_object_id.empty()) {
        out << " related=" << record.related_object_id;
    } else if (record.event.related_object != nullptr) {
        out << " related=" << stable_object_id(record.event.related_object);
    }
    if (record.event.has_success) {
        out << " success=" << (record.event.success ? "true" : "false");
    }
    if (!record.event.detail.empty()) {
        out << " detail=" << record.event.detail;
    }
    return out.str();
}

enum class EventDependencyEdgeKind {
    stream_thread_order,
    stream_resource_order,
    cross_stream_thread_order,
    cross_stream_resource_order
};

inline const char* event_dependency_edge_kind_name(EventDependencyEdgeKind kind) {
    switch (kind) {
        case EventDependencyEdgeKind::stream_thread_order: return "stream_thread_order";
        case EventDependencyEdgeKind::stream_resource_order: return "stream_resource_order";
        case EventDependencyEdgeKind::cross_stream_thread_order: return "cross_stream_thread_order";
        case EventDependencyEdgeKind::cross_stream_resource_order: return "cross_stream_resource_order";
    }
    return "unknown";
}

struct EventDependencyNode {
    std::size_t index = 0;
    std::string stream;
    std::size_t sequence = 0;
    std::size_t event_index = 0;
    int thread_id = -1;
    std::optional<OperationContext> operation;
    std::string kind;
    std::string resource_id;
    std::string related_resource_id;
};

struct EventDependencyEdge {
    std::size_t from = 0;
    std::size_t to = 0;
    EventDependencyEdgeKind kind = EventDependencyEdgeKind::stream_thread_order;
    std::string resource_id;
};

struct EventDependencyGraph {
    std::vector<EventDependencyNode> nodes;
    std::vector<EventDependencyEdge> edges;

    bool empty() const {
        return nodes.empty() && edges.empty();
    }
};

struct EventDependencyAnalysis {
    bool consistent = true;
    std::string explanation;
    std::vector<std::size_t> topological_order;
};

struct OperationDependencyFootprint {
    OperationContext operation;
    std::size_t first_event_index = 0;
    std::size_t last_event_index = 0;
    std::size_t event_count = 0;
    std::vector<std::string> streams;
    std::vector<std::string> resources;
};

inline std::string to_string(const EventDependencyNode& node) {
    std::ostringstream out;
    out << "#" << node.index
        << " " << node.stream << "#" << node.sequence
        << " event=" << node.event_index
        << " thread=" << node.thread_id
        << " kind=" << node.kind;
    append_operation_context_label(out, node.operation);
    if (!node.resource_id.empty()) {
        out << " resource=" << node.resource_id;
    }
    if (!node.related_resource_id.empty()) {
        out << " related=" << node.related_resource_id;
    }
    return out.str();
}

inline std::string to_string(const OperationDependencyFootprint& footprint) {
    std::ostringstream out;
    out << operation_context_label(footprint.operation)
        << " thread=" << footprint.operation.thread_id
        << " events=" << footprint.event_count
        << " event_range=" << footprint.first_event_index << ".." << footprint.last_event_index;
    if (!footprint.streams.empty()) {
        out << " streams=";
        for (std::size_t i = 0; i < footprint.streams.size(); ++i) {
            if (i != 0) out << ",";
            out << footprint.streams[i];
        }
    }
    if (!footprint.resources.empty()) {
        out << " resources=";
        for (std::size_t i = 0; i < footprint.resources.size(); ++i) {
            if (i != 0) out << ",";
            out << footprint.resources[i];
        }
    }
    return out.str();
}

inline std::string to_string(const EventDependencyEdge& edge) {
    std::ostringstream out;
    out << "#" << edge.from << " -> #" << edge.to
        << " kind=" << event_dependency_edge_kind_name(edge.kind);
    if (!edge.resource_id.empty()) {
        out << " resource=" << edge.resource_id;
    }
    return out.str();
}

namespace detail {

inline std::string event_dependency_json_escape(std::string_view value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u"
                        << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

inline std::string event_dependency_dot_escape(std::string_view value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': break;
            case '\t': out << "    "; break;
            default:
                if (ch < 0x20) {
                    out << ' ';
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

inline std::string event_dependency_dot_graph_name(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char ch : name) {
        if (std::isalnum(ch) != 0 || ch == '_') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) {
        result = "event_dependencies";
    }
    if (std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
        result.insert(result.begin(), '_');
    }
    return result;
}

} // namespace detail

inline std::string format_event_dependency_graph_json(
    const EventDependencyGraph& graph,
    const EventDependencyAnalysis* analysis = nullptr
) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"nodes\": [\n";
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const auto& node = graph.nodes[i];
        out << "    {"
            << "\"index\": " << node.index
            << ", \"stream\": \"" << detail::event_dependency_json_escape(node.stream) << "\""
            << ", \"sequence\": " << node.sequence
            << ", \"event_index\": " << node.event_index
            << ", \"thread_id\": " << node.thread_id
            << ", \"kind\": \"" << detail::event_dependency_json_escape(node.kind) << "\""
            << ", \"operation\": \"" << detail::event_dependency_json_escape(node.operation ? operation_context_label(*node.operation) : "") << "\""
            << ", \"resource_id\": \"" << detail::event_dependency_json_escape(node.resource_id) << "\""
            << ", \"related_resource_id\": \"" << detail::event_dependency_json_escape(node.related_resource_id) << "\""
            << "}";
        if (i + 1 != graph.nodes.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"edges\": [\n";
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const auto& edge = graph.edges[i];
        out << "    {"
            << "\"from\": " << edge.from
            << ", \"to\": " << edge.to
            << ", \"kind\": \"" << event_dependency_edge_kind_name(edge.kind) << "\""
            << ", \"resource_id\": \"" << detail::event_dependency_json_escape(edge.resource_id) << "\""
            << "}";
        if (i + 1 != graph.edges.size()) out << ",";
        out << "\n";
    }
    out << "  ]";
    if (analysis != nullptr) {
        out << ",\n";
        out << "  \"analysis\": {\n";
        out << "    \"consistent\": " << (analysis->consistent ? "true" : "false") << ",\n";
        out << "    \"explanation\": \"" << detail::event_dependency_json_escape(analysis->explanation) << "\",\n";
        out << "    \"topological_order\": [";
        for (std::size_t i = 0; i < analysis->topological_order.size(); ++i) {
            if (i != 0) out << ", ";
            out << analysis->topological_order[i];
        }
        out << "]\n";
        out << "  }";
    }
    out << "\n";
    out << "}\n";
    return out.str();
}

inline std::string format_event_dependency_graph_dot(
    const EventDependencyGraph& graph,
    std::string_view graph_name = "event_dependencies"
) {
    std::ostringstream out;
    out << "digraph " << detail::event_dependency_dot_graph_name(graph_name) << " {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box];\n";
    for (const auto& node : graph.nodes) {
        std::ostringstream label;
        label << "#" << node.index
              << "\n" << node.stream << "#" << node.sequence
              << "\nevent=" << node.event_index
              << "\nthread=" << node.thread_id
              << "\nkind=" << node.kind;
        if (node.operation) {
            label << "\noperation=" << operation_context_label(*node.operation);
        }
        if (!node.resource_id.empty()) {
            label << "\nresource=" << node.resource_id;
        }
        if (!node.related_resource_id.empty()) {
            label << "\nrelated=" << node.related_resource_id;
        }
        out << "  n" << node.index
            << " [label=\"" << detail::event_dependency_dot_escape(label.str()) << "\"];\n";
    }
    for (const auto& edge : graph.edges) {
        std::ostringstream label;
        label << event_dependency_edge_kind_name(edge.kind);
        if (!edge.resource_id.empty()) {
            label << "\n" << edge.resource_id;
        }
        out << "  n" << edge.from << " -> n" << edge.to
            << " [label=\"" << detail::event_dependency_dot_escape(label.str()) << "\"];\n";
    }
    out << "}\n";
    return out.str();
}

inline EventDependencyAnalysis analyze_event_dependency_graph(const EventDependencyGraph& graph) {
    EventDependencyAnalysis analysis;
    if (graph.nodes.empty()) {
        analysis.explanation = "event dependency graph is empty";
        return analysis;
    }

    std::vector<std::vector<std::size_t>> outgoing(graph.nodes.size());
    std::vector<std::size_t> indegree(graph.nodes.size(), 0);
    for (const auto& edge : graph.edges) {
        if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) {
            analysis.consistent = false;
            analysis.explanation =
                "event dependency edge references missing node: " + to_string(edge);
            return analysis;
        }
        outgoing[edge.from].push_back(edge.to);
        ++indegree[edge.to];
    }

    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) ready.push_back(i);
    }

    while (!ready.empty()) {
        const auto node = ready.front();
        ready.pop_front();
        analysis.topological_order.push_back(node);
        for (const auto next : outgoing[node]) {
            if (--indegree[next] == 0) {
                ready.push_back(next);
            }
        }
    }

    if (analysis.topological_order.size() != graph.nodes.size()) {
        analysis.consistent = false;
        std::ostringstream out;
        out << "event dependency graph contains a cycle or unresolved dependency; unresolved nodes:";
        for (std::size_t i = 0; i < indegree.size(); ++i) {
            if (indegree[i] != 0) out << " #" << i;
        }
        analysis.explanation = out.str();
        return analysis;
    }

    analysis.explanation =
        "event dependency graph is acyclic; topological nodes=" +
        std::to_string(analysis.topological_order.size()) +
        " edges=" + std::to_string(graph.edges.size());
    return analysis;
}

inline std::vector<OperationDependencyFootprint> build_operation_dependency_footprints(const EventDependencyGraph& graph) {
    std::vector<OperationDependencyFootprint> footprints;

    auto append_unique = [](std::vector<std::string>& values, const std::string& value) {
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    };

    for (const auto& node : graph.nodes) {
        if (!node.operation) continue;

        auto it = std::find_if(footprints.begin(), footprints.end(), [&](const auto& footprint) {
            return same_operation_context_identity(footprint.operation, *node.operation);
        });
        if (it == footprints.end()) {
            footprints.push_back(OperationDependencyFootprint{
                .operation = *node.operation,
                .first_event_index = node.event_index,
                .last_event_index = node.event_index,
                .event_count = 0,
                .streams = {},
                .resources = {}
            });
            it = footprints.end();
            --it;
        }

        it->first_event_index = std::min(it->first_event_index, node.event_index);
        it->last_event_index = std::max(it->last_event_index, node.event_index);
        ++it->event_count;
        append_unique(it->streams, node.stream);
        append_unique(it->resources, node.resource_id);
        append_unique(it->resources, node.related_resource_id);
    }

    for (auto& footprint : footprints) {
        std::sort(footprint.streams.begin(), footprint.streams.end());
        std::sort(footprint.resources.begin(), footprint.resources.end());
    }
    std::stable_sort(footprints.begin(), footprints.end(), [](const auto& left, const auto& right) {
        if (left.first_event_index != right.first_event_index) {
            return left.first_event_index < right.first_event_index;
        }
        return operation_context_label(left.operation) < operation_context_label(right.operation);
    });
    return footprints;
}

namespace detail {

inline void add_event_dependency_node(
    EventDependencyGraph& graph,
    std::string stream,
    std::size_t sequence,
    std::size_t event_index,
    int thread_id,
    std::string kind,
    std::string resource_id = {},
    std::string related_resource_id = {},
    const OperationContext* operation = nullptr
) {
    graph.nodes.push_back(EventDependencyNode{
        .index = graph.nodes.size(),
        .stream = std::move(stream),
        .sequence = sequence,
        .event_index = event_index,
        .thread_id = thread_id,
        .operation = operation == nullptr ? std::optional<OperationContext>{} : std::optional<OperationContext>{*operation},
        .kind = std::move(kind),
        .resource_id = std::move(resource_id),
        .related_resource_id = std::move(related_resource_id)
    });
}

inline void add_event_dependency_edge(
    EventDependencyGraph& graph,
    std::size_t from,
    std::size_t to,
    EventDependencyEdgeKind kind,
    std::string resource_id = {}
) {
    graph.edges.push_back(EventDependencyEdge{
        .from = from,
        .to = to,
        .kind = kind,
        .resource_id = std::move(resource_id)
    });
}

inline void add_stream_dependency_edges(EventDependencyGraph& graph, std::size_t first, std::size_t last) {
    std::unordered_map<int, std::size_t> last_by_thread;
    std::unordered_map<std::string, std::size_t> last_by_resource;

    auto record_resource = [&](const std::string& resource, std::size_t node_index) {
        if (resource.empty()) return;
        auto [it, inserted] = last_by_resource.emplace(resource, node_index);
        if (!inserted) {
            add_event_dependency_edge(
                graph,
                it->second,
                node_index,
                EventDependencyEdgeKind::stream_resource_order,
                resource
            );
            it->second = node_index;
        }
    };

    for (std::size_t index = first; index < last; ++index) {
        const auto& node = graph.nodes[index];
        if (node.thread_id >= 0) {
            auto [it, inserted] = last_by_thread.emplace(node.thread_id, index);
            if (!inserted) {
                add_event_dependency_edge(
                    graph,
                    it->second,
                    index,
                    EventDependencyEdgeKind::stream_thread_order
                );
                it->second = index;
            }
        }
        record_resource(node.resource_id, index);
        record_resource(node.related_resource_id, index);
    }
}

inline bool event_dependency_node_precedes(const EventDependencyNode& left, const EventDependencyNode& right) {
    if (left.event_index != right.event_index) {
        return left.event_index < right.event_index;
    }
    return left.index < right.index;
}

inline void add_cross_stream_dependency_edges(EventDependencyGraph& graph) {
    std::vector<std::size_t> order;
    order.reserve(graph.nodes.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        order.push_back(index);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return event_dependency_node_precedes(graph.nodes[left], graph.nodes[right]);
    });

    std::unordered_map<int, std::size_t> last_by_thread;
    std::unordered_map<std::string, std::size_t> last_by_resource;

    auto add_if_cross_stream = [&](std::size_t from, std::size_t to, EventDependencyEdgeKind kind, std::string resource = {}) {
        if (from == to || graph.nodes[from].stream == graph.nodes[to].stream) return;
        add_event_dependency_edge(graph, from, to, kind, std::move(resource));
    };

    auto record_resource = [&](const std::string& resource, std::size_t node_index) {
        if (resource.empty()) return;
        auto [it, inserted] = last_by_resource.emplace(resource, node_index);
        if (!inserted) {
            add_if_cross_stream(
                it->second,
                node_index,
                EventDependencyEdgeKind::cross_stream_resource_order,
                resource
            );
            it->second = node_index;
        }
    };

    for (const auto node_index : order) {
        const auto& node = graph.nodes[node_index];
        if (node.thread_id >= 0) {
            auto [it, inserted] = last_by_thread.emplace(node.thread_id, node_index);
            if (!inserted) {
                add_if_cross_stream(
                    it->second,
                    node_index,
                    EventDependencyEdgeKind::cross_stream_thread_order
                );
                it->second = node_index;
            }
        }
        record_resource(node.resource_id, node_index);
        record_resource(node.related_resource_id, node_index);
    }
}

inline std::string stm_dependency_resource_id(const StmEventRecord& record) {
    if (!record.address_id.empty()) return record.address_id;
    if (record.has_lock_slot) return "lock_slot#" + std::to_string(record.lock_slot);
    if (record.transaction_id != 0) return "tx#" + std::to_string(record.transaction_id);
    return {};
}

inline std::string trace_dependency_resource_id(const TraceEventRecord& record) {
    if (record.transaction_id != 0) return "tx#" + std::to_string(record.transaction_id);
    return {};
}

} // namespace detail

inline EventDependencyGraph build_event_dependency_graph(
    const std::vector<TraceEventRecord>& trace_events,
    const std::vector<MemoryEventRecord>& memory_events,
    const std::vector<StmEventRecord>& stm_events,
    const std::vector<SourceAccessEventRecord>& source_accesses,
    const std::vector<SynchronizationEventRecord>& synchronization_events
) {
    EventDependencyGraph graph;

    const auto trace_first = graph.nodes.size();
    for (const auto& event : trace_events) {
        detail::add_event_dependency_node(
            graph,
            "trace",
            event.sequence,
            event.event_index,
            event.thread_id,
            event.kind.empty() ? event.description : event.kind,
            detail::trace_dependency_resource_id(event),
            {},
            event.operation ? &*event.operation : nullptr
        );
    }
    detail::add_stream_dependency_edges(graph, trace_first, graph.nodes.size());

    const auto memory_first = graph.nodes.size();
    for (const auto& event : memory_events) {
        detail::add_event_dependency_node(
            graph,
            "memory",
            event.sequence,
            event.event_index,
            event.thread_id,
            memory_event_kind_name(event.event.kind),
            event.object_id,
            {},
            event.operation ? &*event.operation : nullptr
        );
    }
    detail::add_stream_dependency_edges(graph, memory_first, graph.nodes.size());

    const auto stm_first = graph.nodes.size();
    for (const auto& event : stm_events) {
        detail::add_event_dependency_node(
            graph,
            "stm",
            event.sequence,
            event.event_index,
            event.thread_id,
            event.kind.empty() ? event.description : event.kind,
            detail::stm_dependency_resource_id(event),
            {},
            event.operation ? &*event.operation : nullptr
        );
    }
    detail::add_stream_dependency_edges(graph, stm_first, graph.nodes.size());

    const auto source_first = graph.nodes.size();
    for (const auto& event : source_accesses) {
        detail::add_event_dependency_node(
            graph,
            "source",
            event.sequence,
            event.event_index,
            event.thread_id,
            source_access_kind_name(event.event.kind),
            event.object_id,
            event.location_id,
            event.operation ? &*event.operation : nullptr
        );
    }
    detail::add_stream_dependency_edges(graph, source_first, graph.nodes.size());

    const auto sync_first = graph.nodes.size();
    for (const auto& event : synchronization_events) {
        detail::add_event_dependency_node(
            graph,
            "synchronization",
            event.sequence,
            event.event_index,
            event.thread_id,
            synchronization_event_kind_name(event.event.kind),
            event.object_id,
            event.related_object_id,
            event.operation ? &*event.operation : nullptr
        );
    }
    detail::add_stream_dependency_edges(graph, sync_first, graph.nodes.size());
    detail::add_cross_stream_dependency_edges(graph);

    return graph;
}

inline EventDependencyGraph build_event_dependency_graph(
    const std::vector<MemoryEventRecord>& memory_events,
    const std::vector<StmEventRecord>& stm_events,
    const std::vector<SourceAccessEventRecord>& source_accesses,
    const std::vector<SynchronizationEventRecord>& synchronization_events
) {
    return build_event_dependency_graph(
        std::vector<TraceEventRecord>{},
        memory_events,
        stm_events,
        source_accesses,
        synchronization_events
    );
}

struct CheckStats {
    int scenarios_generated = 0;
    int schedules_generated = 0;
    int schedules_explored = 0;
    int schedules_pruned_by_context_bound = 0;
    int schedules_pruned_by_invocation_budget = 0;
    int schedules_pruned_by_operation_context = 0;
    int schedules_pruned_by_event_dependency = 0;
    int verifications_pruned_by_duplicate_history = 0;
    int context_switch_depth_increases = 0;
    int max_context_switch_depth_explored = 0;
};

enum class FailureKind {
    none,
    invalid_results,
    validation_failure,
    unexpected_exception,
    deadlock,
    livelock,
    obstruction_freedom,
    timeout
};

inline const char* failure_kind_name(FailureKind kind) {
    switch (kind) {
        case FailureKind::none: return "none";
        case FailureKind::invalid_results: return "invalid_results";
        case FailureKind::validation_failure: return "validation_failure";
        case FailureKind::unexpected_exception: return "unexpected_exception";
        case FailureKind::deadlock: return "deadlock";
        case FailureKind::livelock: return "livelock";
        case FailureKind::obstruction_freedom: return "obstruction_freedom";
        case FailureKind::timeout: return "timeout";
    }
    return "unknown";
}

struct CheckResult {
    bool success = true;
    FailureKind failure = FailureKind::none;
    std::string test_name;
    std::string message;
    ExecutionScenario scenario;
    std::vector<int> schedule;
    std::vector<ScheduleDecision> schedule_decisions;
    ExecutionResult execution_result;
    std::string trace;
    std::string state_representation;
    std::string verifier_explanation;
    std::exception_ptr exception;
    std::vector<std::string> warnings;
    std::vector<TraceEventRecord> trace_events;
    std::vector<MemoryEventRecord> memory_events;
    std::vector<StmEventRecord> stm_events;
    std::vector<SourceAccessEventRecord> source_accesses;
    std::vector<SynchronizationEventRecord> synchronization_events;
    EventDependencyGraph event_dependencies;
    EventDependencyAnalysis event_dependency_analysis;
    std::vector<OperationDependencyFootprint> operation_dependency_footprints;
    CheckStats stats;
};

namespace detail {

inline void append_unique_warning(std::vector<std::string>& warnings, const std::string& warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

inline void append_unique_warnings(std::vector<std::string>& warnings, const std::vector<std::string>& additional) {
    for (const auto& warning : additional) {
        append_unique_warning(warnings, warning);
    }
}

inline void refresh_event_dependencies(CheckResult& result) {
    result.event_dependencies = build_event_dependency_graph(
        result.trace_events,
        result.memory_events,
        result.stm_events,
        result.source_accesses,
        result.synchronization_events
    );
    result.event_dependency_analysis = analyze_event_dependency_graph(result.event_dependencies);
    result.operation_dependency_footprints = build_operation_dependency_footprints(result.event_dependencies);
}

inline std::string trim_ascii_whitespace(std::string text) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };

    const auto first = std::find_if_not(text.begin(), text.end(), [&](char c) {
        return is_space(static_cast<unsigned char>(c));
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [&](char c) {
        return is_space(static_cast<unsigned char>(c));
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

inline std::optional<int> parse_non_negative_int_token(const std::string& token) {
    if (token.empty() || token[0] == '-') return std::nullopt;
    std::istringstream input(token);
    int value = 0;
    char extra = '\0';
    if (!(input >> value)) return std::nullopt;
    if (input >> extra) return std::nullopt;
    if (value < 0) return std::nullopt;
    return value;
}

inline void annotate_named_check(CheckResult& result, const std::string& name) {
    result.test_name = name;
    if (!name.empty() && !result.trace.empty()) {
        result.trace = "test: " + name + "\n" + result.trace;
    }
}

inline void append_history_key_token(std::ostringstream& out, std::string_view value) {
    out << value.size() << ':' << value << ';';
}

inline std::optional<std::string> public_history_equivalence_key(
    const ExecutionScenario& scenario,
    const ExecutionResult& result
) {
    struct Row {
        std::uint64_t invocation_clock = 0;
        std::uint64_t response_clock = 0;
        std::string label;
        std::string actor;
        std::string response;
    };

    std::vector<Row> rows;

    auto add_rows = [&](std::string_view phase, const std::vector<Actor>& actors, const std::vector<OperationInterval>& intervals) -> bool {
        if (actors.size() != intervals.size()) return false;
        for (std::size_t i = 0; i < actors.size(); ++i) {
            rows.push_back(Row{
                .invocation_clock = intervals[i].invocation_clock,
                .response_clock = intervals[i].response_clock,
                .label = std::string(phase) + " " + std::to_string(i),
                .actor = actors[i].to_string(),
                .response = intervals[i].result.to_string()
            });
        }
        return true;
    };

    if (!add_rows("init", scenario.init, result.init_intervals)) return std::nullopt;
    if (scenario.parallel.size() != result.parallel_intervals.size()) return std::nullopt;
    for (std::size_t thread = 0; thread < scenario.parallel.size(); ++thread) {
        const auto& actors = scenario.parallel[thread];
        const auto& intervals = result.parallel_intervals[thread];
        if (actors.size() != intervals.size()) return std::nullopt;
        for (std::size_t actor = 0; actor < actors.size(); ++actor) {
            rows.push_back(Row{
                .invocation_clock = intervals[actor].invocation_clock,
                .response_clock = intervals[actor].response_clock,
                .label = "thread " + std::to_string(thread) + " actor " + std::to_string(actor),
                .actor = actors[actor].to_string(),
                .response = intervals[actor].result.to_string()
            });
        }
    }
    if (!add_rows("post", scenario.post, result.post_intervals)) return std::nullopt;

    std::ostringstream out;
    out << "public-history-v1;";
    out << "rows=" << rows.size() << ';';
    for (const auto& row : rows) {
        append_history_key_token(out, row.label);
        append_history_key_token(out, row.actor);
        append_history_key_token(out, row.response);
    }

    out << "real-time:";
    for (std::size_t left = 0; left < rows.size(); ++left) {
        for (std::size_t right = 0; right < rows.size(); ++right) {
            if (left != right && rows[left].response_clock < rows[right].invocation_clock) {
                out << left << '<' << right << ',';
            }
        }
    }
    out << ';';
    return out.str();
}

template <typename Builder>
TestSpec build_test_spec(Builder&& builder) {
    using Result = std::invoke_result_t<Builder&&>;
    if constexpr (std::is_void_v<Result>) {
        static_assert(always_false_v<Builder>, "named check builder must return TestSpec or TestBuilder");
    } else {
        return TestSpec(std::invoke(std::forward<Builder>(builder)));
    }
}

inline void validate_replay_schedule(const ExecutionScenario& scenario, const std::vector<int>& schedule) {
    if (!scenario.valid()) {
        throw std::invalid_argument("lincheck replay scenario must contain at least one parallel actor");
    }
    if (schedule.empty()) {
        throw std::invalid_argument("lincheck replay schedule must contain at least one thread choice");
    }
    for (const int choice : schedule) {
        if (choice < 0 || static_cast<std::size_t>(choice) >= scenario.parallel.size()) {
            throw std::invalid_argument(
                "lincheck replay schedule choice " + std::to_string(choice) +
                " is outside the scenario thread range [0, " +
                std::to_string(scenario.parallel.empty() ? 0 : scenario.parallel.size() - 1) +
                "]"
            );
        }
    }
}

inline std::vector<int> schedule_from_decisions(const ExecutionScenario& scenario, const std::vector<ScheduleDecision>& decisions) {
    if (!scenario.valid()) {
        throw std::invalid_argument("lincheck replay scenario must contain at least one parallel actor");
    }
    if (decisions.empty()) {
        throw std::invalid_argument("lincheck replay schedule decisions must contain at least one decision");
    }

    std::vector<int> schedule;
    schedule.reserve(decisions.size());
    for (std::size_t i = 0; i < decisions.size(); ++i) {
        const auto& decision = decisions[i];
        if (decision.switch_position != i) {
            throw std::invalid_argument("lincheck replay schedule decisions must have contiguous switch positions");
        }
        if (decision.location.empty()) {
            throw std::invalid_argument("lincheck replay schedule decision must include a switch location");
        }
        if (decision.chosen_thread < 0 || static_cast<std::size_t>(decision.chosen_thread) >= scenario.parallel.size()) {
            throw std::invalid_argument("lincheck replay schedule decision chose a thread outside the scenario thread range");
        }
        if (std::find(decision.runnable_threads.begin(), decision.runnable_threads.end(), decision.chosen_thread) == decision.runnable_threads.end()) {
            throw std::invalid_argument("lincheck replay schedule decision chose a thread not listed as runnable");
        }
        for (const int thread : decision.runnable_threads) {
            if (thread < 0 || static_cast<std::size_t>(thread) >= scenario.parallel.size()) {
                throw std::invalid_argument("lincheck replay schedule decision contains a runnable thread outside the scenario thread range");
            }
        }
        schedule.push_back(decision.chosen_thread);
    }
    return schedule;
}

inline void validate_actor_against_spec(
    const TestSpec& spec,
    const Actor& actor,
    const std::string& location
) {
    if (actor.operation_index >= spec.operations.size()) {
        throw std::invalid_argument(
            "lincheck scenario " + location +
            " has operation_index " + std::to_string(actor.operation_index) +
            " outside registered operation range [0, " +
            std::to_string(spec.operations.empty() ? 0 : spec.operations.size() - 1) +
            "]"
        );
    }

    const auto& operation = spec.operations[actor.operation_index];
    if (actor.arguments.size() != operation.argument_count) {
        throw std::invalid_argument(
            "lincheck scenario " + location +
            " operation " + operation.name +
            " has " + std::to_string(actor.arguments.size()) +
            " arguments but expected " + std::to_string(operation.argument_count)
        );
    }
}

inline std::string non_parallel_key_for_operation(const Operation& operation) {
    if (!operation.options.group.empty()) return operation.options.group;
    return operation.name;
}

inline void validate_scenario_against_spec(const TestSpec& spec, const ExecutionScenario& scenario) {
    if (spec.operations.empty()) {
        throw std::invalid_argument("lincheck test spec must register at least one operation");
    }

    std::unordered_set<std::size_t> used_one_shot_operations;
    std::unordered_map<std::string, std::size_t> non_parallel_group_thread;

    auto validate_constraints = [&](const Actor& actor, const std::string& location, std::optional<std::size_t> parallel_thread) {
        const auto& operation = spec.operations[actor.operation_index];
        if (operation.options.one_shot) {
            if (!used_one_shot_operations.insert(actor.operation_index).second) {
                throw std::invalid_argument(
                    "lincheck scenario " + location +
                    " repeats one-shot operation " + operation.name
                );
            }
        }

        if (parallel_thread && operation.options.non_parallel) {
            const auto key = non_parallel_key_for_operation(operation);
            auto [it, inserted] = non_parallel_group_thread.emplace(key, *parallel_thread);
            if (!inserted && it->second != *parallel_thread) {
                throw std::invalid_argument(
                    "lincheck scenario " + location +
                    " places non-parallel group " + key +
                    " in multiple parallel threads"
                );
            }
        }
    };

    for (std::size_t i = 0; i < scenario.init.size(); ++i) {
        const auto location = "init actor " + std::to_string(i);
        validate_actor_against_spec(spec, scenario.init[i], location);
        validate_constraints(scenario.init[i], location, std::nullopt);
    }
    for (std::size_t thread = 0; thread < scenario.parallel.size(); ++thread) {
        for (std::size_t i = 0; i < scenario.parallel[thread].size(); ++i) {
            const auto location = "parallel thread " + std::to_string(thread) + " actor " + std::to_string(i);
            validate_actor_against_spec(
                spec,
                scenario.parallel[thread][i],
                location
            );
            validate_constraints(scenario.parallel[thread][i], location, thread);
        }
    }
    for (std::size_t i = 0; i < scenario.post.size(); ++i) {
        const auto location = "post actor " + std::to_string(i);
        validate_actor_against_spec(spec, scenario.post[i], location);
        validate_constraints(scenario.post[i], location, std::nullopt);
    }
}

} // namespace detail

inline std::optional<std::vector<int>> parse_schedule_line(std::string line) {
    line = detail::trim_ascii_whitespace(std::move(line));
    static constexpr std::string_view prefix = "schedule:";
    if (line.rfind(prefix, 0) != 0) return std::nullopt;

    std::istringstream input(line.substr(prefix.size()));
    std::vector<int> schedule;
    std::string token;
    while (input >> token) {
        auto choice = detail::parse_non_negative_int_token(token);
        if (!choice) return std::nullopt;
        schedule.push_back(*choice);
    }
    if (schedule.empty()) return std::nullopt;
    return schedule;
}

inline std::optional<ScheduleDecision> parse_schedule_decision_line(std::string line) {
    line = detail::trim_ascii_whitespace(std::move(line));
    if (line.empty() || line.front() != '#') return std::nullopt;

    const auto first_space = line.find(' ');
    if (first_space == std::string::npos) return std::nullopt;
    auto position = detail::parse_non_negative_int_token(line.substr(1, first_space - 1));
    if (!position) return std::nullopt;

    std::string rest = detail::trim_ascii_whitespace(line.substr(first_space + 1));
    int thread_id = -1;
    static constexpr std::string_view thread_prefix = "thread ";
    const auto at_separator = rest.find(" @ ");
    if (rest.rfind(thread_prefix, 0) == 0 && at_separator != std::string::npos) {
        auto parsed_thread = detail::parse_non_negative_int_token(
            rest.substr(thread_prefix.size(), at_separator - thread_prefix.size())
        );
        if (!parsed_thread) return std::nullopt;
        thread_id = *parsed_thread;
        rest = rest.substr(at_separator + 3);
    }

    const auto arrow = rest.find(" -> ");
    const auto runnable = rest.find(" runnable:");
    if (arrow == std::string::npos || runnable == std::string::npos || runnable <= arrow) {
        return std::nullopt;
    }

    auto location = detail::trim_ascii_whitespace(rest.substr(0, arrow));
    auto chosen = detail::parse_non_negative_int_token(rest.substr(arrow + 4, runnable - (arrow + 4)));
    if (!chosen) return std::nullopt;

    std::string runnable_text = rest.substr(runnable + std::string(" runnable:").size());
    if (const auto operations = runnable_text.find(" operations:"); operations != std::string::npos) {
        runnable_text = runnable_text.substr(0, operations);
    }
    std::istringstream input(runnable_text);
    std::vector<int> runnable_threads;
    std::string token;
    while (input >> token) {
        auto parsed = detail::parse_non_negative_int_token(token);
        if (!parsed) return std::nullopt;
        runnable_threads.push_back(*parsed);
    }
    if (location.empty() || runnable_threads.empty()) return std::nullopt;

    return ScheduleDecision{
        .switch_position = static_cast<std::size_t>(*position),
        .thread_id = thread_id,
        .location = std::move(location),
        .runnable_threads = std::move(runnable_threads),
        .chosen_thread = *chosen,
        .runnable_operations = {}
    };
}

inline std::optional<std::vector<int>> schedule_from_trace(const std::string& trace) {
    std::istringstream input(trace);
    std::string line;
    while (std::getline(input, line)) {
        auto schedule = parse_schedule_line(line);
        if (schedule) return schedule;
    }
    return std::nullopt;
}

inline std::optional<std::vector<ScheduleDecision>> schedule_decisions_from_trace(const std::string& trace) {
    std::istringstream input(trace);
    std::string line;
    bool in_decisions = false;
    std::vector<ScheduleDecision> decisions;
    while (std::getline(input, line)) {
        const auto trimmed = detail::trim_ascii_whitespace(line);
        if (trimmed == "schedule decisions:") {
            in_decisions = true;
            continue;
        }
        if (!in_decisions) continue;
        auto decision = parse_schedule_decision_line(trimmed);
        if (!decision) break;
        if (decision->switch_position != decisions.size()) return std::nullopt;
        decisions.push_back(std::move(*decision));
    }
    if (decisions.empty()) return std::nullopt;
    return decisions;
}

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LivelockError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ScheduleAbortError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ReplayScheduleError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class RandomExecutionGenerator {
public:
    RandomExecutionGenerator(
        const TestSpec& spec,
        int threads,
        int actors_per_thread,
        int actors_before,
        int actors_after,
        std::uint64_t seed
    ) : spec_(spec),
        threads_(threads),
        actors_per_thread_(actors_per_thread),
        actors_before_(actors_before),
        actors_after_(actors_after),
        rng_(seed) {
        if (threads_ < 1) {
            throw std::invalid_argument("lincheck option threads must be >= 1");
        }
        if (actors_per_thread_ < 1) {
            throw std::invalid_argument("lincheck option actors_per_thread must be >= 1");
        }
        if (actors_before_ < 0) {
            throw std::invalid_argument("lincheck option actors_before must be >= 0");
        }
        if (actors_after_ < 0) {
            throw std::invalid_argument("lincheck option actors_after must be >= 0");
        }
    }

    ExecutionScenario next() {
        GenerationState state;
        reset_generators(state);
        ExecutionScenario scenario;
        scenario.init = generate_actor_list(actors_before_, state, false);
        scenario.parallel.resize(static_cast<std::size_t>(threads_));
        for (std::size_t thread_index = 0; thread_index < scenario.parallel.size(); ++thread_index) {
            scenario.parallel[thread_index] = generate_actor_list(actors_per_thread_, state, true, thread_index);
        }
        scenario.post = generate_actor_list(actors_after_, state, false);
        return scenario;
    }

private:
    struct GenerationState {
        std::unordered_set<std::size_t> used_one_shot_operations;
        std::unordered_map<std::string, std::size_t> parallel_non_parallel_group_thread;
        std::vector<std::vector<Generator>> generators;
    };

    void reset_generators(GenerationState& state) const {
        state.generators.clear();
        state.generators.reserve(spec_.operations.size());
        for (const auto& operation : spec_.operations) {
            auto generators = operation.generators;
            for (auto& generator : generators) {
                generator.reset();
            }
            state.generators.push_back(std::move(generators));
        }
    }

    Actor make_actor(std::size_t op_index, GenerationState& state) {
        const auto& op = spec_.operations[op_index];
        Actor actor;
        actor.operation_index = op_index;
        actor.name = op.name;
        actor.group = op.options.group;
        actor.non_parallel = op.options.non_parallel;
        actor.one_shot = op.options.one_shot;
        actor.exception_results = op.options.exception_results;
        for (auto& generator : state.generators[op_index]) {
            actor.arguments.push_back(generator(rng_));
        }
        return actor;
    }

    std::string non_parallel_key(std::size_t op_index) const {
        const auto& op = spec_.operations[op_index];
        if (!op.options.group.empty()) return op.options.group;
        return op.name;
    }

    bool eligible(
        std::size_t op_index,
        const GenerationState& state,
        bool parallel_section,
        std::size_t parallel_thread = 0
    ) const {
        const auto& op = spec_.operations[op_index];
        if (op.options.one_shot && state.used_one_shot_operations.contains(op_index)) {
            return false;
        }
        if (parallel_section && op.options.non_parallel) {
            const auto owner = state.parallel_non_parallel_group_thread.find(non_parallel_key(op_index));
            if (owner != state.parallel_non_parallel_group_thread.end() && owner->second != parallel_thread) {
                return false;
            }
        }
        return true;
    }

    void mark_generated(
        std::size_t op_index,
        GenerationState& state,
        bool parallel_section,
        std::size_t parallel_thread = 0
    ) const {
        const auto& op = spec_.operations[op_index];
        if (op.options.one_shot) {
            state.used_one_shot_operations.insert(op_index);
        }
        if (parallel_section && op.options.non_parallel) {
            state.parallel_non_parallel_group_thread.emplace(non_parallel_key(op_index), parallel_thread);
        }
    }

    std::optional<Actor> generate_actor(
        GenerationState& state,
        bool parallel_section,
        std::size_t parallel_thread = 0
    ) {
        if (spec_.operations.empty()) {
            throw std::invalid_argument("no operations registered");
        }

        std::vector<std::size_t> candidates;
        candidates.reserve(spec_.operations.size());
        for (std::size_t i = 0; i < spec_.operations.size(); ++i) {
            if (eligible(i, state, parallel_section, parallel_thread)) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            return std::nullopt;
        }

        std::uniform_int_distribution<std::size_t> op_dist(0, candidates.size() - 1);
        const auto op_index = candidates[op_dist(rng_)];
        mark_generated(op_index, state, parallel_section, parallel_thread);
        return make_actor(op_index, state);
    }

    std::vector<Actor> generate_actor_list(
        int count,
        GenerationState& state,
        bool parallel_section,
        std::size_t parallel_thread = 0
    ) {
        std::vector<Actor> actors;
        actors.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i) {
            auto actor = generate_actor(state, parallel_section, parallel_thread);
            if (!actor) break;
            actors.push_back(std::move(*actor));
        }
        return actors;
    }

    const TestSpec& spec_;
    int threads_;
    int actors_per_thread_;
    int actors_before_;
    int actors_after_;
    std::mt19937_64 rng_;
};

class LinearizabilityVerifier {
public:
    explicit LinearizabilityVerifier(const TestSpec& spec) : spec_(spec) {}

    struct VerificationReport {
        bool success = true;
        std::string explanation;
    };

    bool verify(const ExecutionScenario& scenario, const ExecutionResult& results) const {
        detail::validate_scenario_against_spec(spec_, scenario);
        auto model = spec_.make_sequential();
        if (!apply_sequence(model.get(), scenario.init, results.init_results, "init")) return false;
        VerifierCache cache;
        std::shared_ptr<void> final_parallel_model;
        if (!verify_parallel(
            model,
            scenario,
            results,
            std::vector<std::size_t>(scenario.parallel.size(), 0),
            scenario.post,
            results.post_results,
            cache,
            nullptr,
            &final_parallel_model
        )) {
            return false;
        }
        return true;
    }

    VerificationReport verify_with_report(const ExecutionScenario& scenario, const ExecutionResult& results) const {
        detail::validate_scenario_against_spec(spec_, scenario);
        auto model = spec_.make_sequential();
        VerificationReport report;
        if (!apply_sequence(model.get(), scenario.init, results.init_results, "init", &report.explanation)) {
            report.success = false;
            return report;
        }
        VerifierCache cache;
        std::shared_ptr<void> final_parallel_model;
        if (!verify_parallel(
            model,
            scenario,
            results,
            std::vector<std::size_t>(scenario.parallel.size(), 0),
            scenario.post,
            results.post_results,
            cache,
            &report.explanation,
            &final_parallel_model
        )) {
            report.success = false;
            return report;
        }
        report.explanation = "history is linearizable";
        return report;
    }

private:
    struct CacheKey {
        std::vector<std::size_t> positions;
        Value state;

        friend bool operator==(const CacheKey&, const CacheKey&) = default;
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const noexcept {
            std::uint64_t hash = key.state.stable_hash();
            auto mix = [&](std::uint64_t value) {
                hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            };
            mix(static_cast<std::uint64_t>(key.positions.size()));
            for (const auto position : key.positions) {
                mix(static_cast<std::uint64_t>(position));
            }
            return static_cast<std::size_t>(hash);
        }
    };

    struct VerifierCache {
        std::unordered_map<CacheKey, std::shared_ptr<void>, CacheKeyHash> successful;
        std::unordered_set<CacheKey, CacheKeyHash> failed;
    };

    bool apply_sequence(
        void* model,
        const std::vector<Actor>& actors,
        const std::vector<Value>& expected,
        const char* section,
        std::string* explanation = nullptr
    ) const {
        if (actors.size() != expected.size()) {
            if (explanation) {
                *explanation = std::string(section) + " section result count mismatch: " +
                    std::to_string(actors.size()) + " actors but " +
                    std::to_string(expected.size()) + " observed results";
            }
            return false;
        }
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const auto actual = run_sequential(model, actors[i]);
            if (!(actual == expected[i])) {
                if (explanation) {
                    *explanation = std::string(section) + " operation " + std::to_string(i) +
                        " " + actors[i].to_string() +
                        " observed " + expected[i].to_string() +
                        " but sequential model returned " + actual.to_string();
                }
                return false;
            }
        }
        return true;
    }

    bool verify_parallel(
        const std::shared_ptr<void>& model,
        const ExecutionScenario& scenario,
        const ExecutionResult& results,
        std::vector<std::size_t> positions,
        const std::vector<Actor>& post_actors,
        const std::vector<Value>& post_results,
        VerifierCache& cache,
        std::string* explanation = nullptr,
        std::shared_ptr<void>* final_model = nullptr
    ) const {
        bool complete = true;
        for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
            if (positions[t] < scenario.parallel[t].size()) {
                complete = false;
                break;
            }
        }
        if (complete) {
            auto completed_model = model;
            if (!post_actors.empty() || !post_results.empty()) {
                completed_model = spec_.clone_sequential(model);
                if (!apply_sequence(completed_model.get(), post_actors, post_results, "post", explanation)) {
                    return false;
                }
            }
            if (final_model) *final_model = std::move(completed_model);
            return true;
        }

        const auto cache_key = make_cache_key(model, positions);
        if (cache_key) {
            auto success = cache.successful.find(*cache_key);
            if (success != cache.successful.end()) {
                if (!final_model) return true;
                if (success->second) {
                    *final_model = spec_.clone_sequential(success->second);
                    return true;
                }
            }
            if (!explanation && cache.failed.contains(*cache_key)) return false;
        }

        std::vector<std::string> candidate_rejections;
        for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
            if (positions[t] >= scenario.parallel[t].size()) continue;
            const auto candidate = candidate_label(scenario, t, positions[t]);
            if (t >= results.parallel_results.size()) {
                add_candidate_rejection(candidate_rejections, candidate + " has no result vector");
                continue;
            }
            if (positions[t] >= results.parallel_results[t].size()) {
                add_candidate_rejection(candidate_rejections, candidate + " has no observed result");
                continue;
            }
            if (auto blocker = real_time_blocker(scenario, results, positions, t, positions[t])) {
                add_candidate_rejection(
                    candidate_rejections,
                    candidate + " cannot linearize before " + *blocker
                );
                continue;
            }

            auto next_model = spec_.clone_sequential(model);
            const auto& actor = scenario.parallel[t][positions[t]];
            const auto actual = run_sequential(next_model.get(), actor);
            const auto& observed = results.parallel_results[t][positions[t]];
            if (!(actual == observed)) {
                add_candidate_rejection(
                    candidate_rejections,
                    candidate + " observed " + observed.to_string() +
                    " but sequential model returned " + actual.to_string()
                );
                continue;
            }

            auto next_positions = positions;
            ++next_positions[t];
            std::string nested_explanation;
            std::shared_ptr<void> nested_final_model;
            if (verify_parallel(
                next_model,
                scenario,
                results,
                std::move(next_positions),
                post_actors,
                post_results,
                cache,
                explanation ? &nested_explanation : nullptr,
                final_model ? &nested_final_model : nullptr
            )) {
                auto completed_model = nested_final_model ? std::move(nested_final_model) : std::move(next_model);
                if (cache_key) {
                    cache.successful.insert_or_assign(
                        *cache_key,
                        final_model ? spec_.clone_sequential(completed_model) : nullptr
                    );
                }
                if (final_model) {
                    *final_model = std::move(completed_model);
                }
                return true;
            }
            if (explanation) {
                add_candidate_rejection(
                    candidate_rejections,
                    candidate + " matches now, but the remaining suffix has no legal completion: " +
                    first_line(nested_explanation)
                );
            }
        }
        if (cache_key) cache.failed.insert(*cache_key);
        if (explanation) {
            *explanation = format_parallel_failure(positions, candidate_rejections);
        }
        return false;
    }

    std::optional<CacheKey> make_cache_key(
        const std::shared_ptr<void>& model,
        const std::vector<std::size_t>& positions
    ) const {
        if (!spec_.sequential_state_key) return std::nullopt;
        return CacheKey{
            .positions = positions,
            .state = spec_.sequential_state_key(model.get())
        };
    }

    static bool has_complete_intervals(const ExecutionScenario& scenario, const ExecutionResult& results) {
        if (results.parallel_intervals.size() != scenario.parallel.size()) return false;
        for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
            if (results.parallel_intervals[t].size() != scenario.parallel[t].size()) return false;
        }
        return true;
    }

    static bool real_time_predecessors_satisfied(
        const ExecutionScenario& scenario,
        const ExecutionResult& results,
        const std::vector<std::size_t>& positions,
        std::size_t candidate_thread,
        std::size_t candidate_position
    ) {
        return !real_time_blocker(scenario, results, positions, candidate_thread, candidate_position);
    }

    static std::optional<std::string> real_time_blocker(
        const ExecutionScenario& scenario,
        const ExecutionResult& results,
        const std::vector<std::size_t>& positions,
        std::size_t candidate_thread,
        std::size_t candidate_position
    ) {
        if (!has_complete_intervals(scenario, results)) return std::nullopt;
        const auto& candidate = results.parallel_intervals[candidate_thread][candidate_position];
        if (candidate.invocation_clock == 0) return std::nullopt;

        for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
            for (std::size_t p = positions[t]; p < scenario.parallel[t].size(); ++p) {
                if (t == candidate_thread && p == candidate_position) continue;
                const auto& pending = results.parallel_intervals[t][p];
                if (pending.response_clock != 0 && pending.response_clock < candidate.invocation_clock) {
                    return candidate_label(scenario, t, p) +
                        " responded at clock " + std::to_string(pending.response_clock) +
                        " before candidate invocation clock " + std::to_string(candidate.invocation_clock);
                }
            }
        }
        return std::nullopt;
    }

    Value run_sequential(void* model, const Actor& actor) const {
        const auto& op = spec_.operations.at(actor.operation_index);
        try {
            return op.run_sequential(model, actor.arguments);
        } catch (...) {
            if (op.options.exception_results) {
                return detail::exception_result_value(std::current_exception());
            }
            throw;
        }
    }

    static void add_candidate_rejection(std::vector<std::string>& rejections, std::string rejection) {
        static constexpr std::size_t max_rejections = 8;
        if (rejections.size() < max_rejections) {
            rejections.push_back(std::move(rejection));
        } else if (rejections.size() == max_rejections) {
            rejections.push_back("additional candidate rejections omitted");
        }
    }

    static std::string candidate_label(const ExecutionScenario& scenario, std::size_t thread, std::size_t position) {
        std::ostringstream out;
        out << "thread " << thread << " actor " << position << " ";
        if (thread < scenario.parallel.size() && position < scenario.parallel[thread].size()) {
            out << scenario.parallel[thread][position].to_string();
        } else {
            out << "<missing actor>";
        }
        return out.str();
    }

    static std::string first_line(const std::string& text) {
        const auto newline = text.find('\n');
        if (newline == std::string::npos) return text;
        return text.substr(0, newline);
    }

    static std::string positions_string(const std::vector<std::size_t>& positions) {
        std::ostringstream out;
        out << "[";
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (i != 0) out << ", ";
            out << positions[i];
        }
        out << "]";
        return out.str();
    }

    static std::string format_parallel_failure(
        const std::vector<std::size_t>& positions,
        const std::vector<std::string>& candidate_rejections
    ) {
        std::ostringstream out;
        out << "no legal sequential ordering found at parallel positions " << positions_string(positions);
        if (candidate_rejections.empty()) {
            out << "\nno pending candidate operation was available";
        } else {
            out << "\ncandidate rejections:";
            for (const auto& rejection : candidate_rejections) {
                out << "\n- " << rejection;
            }
        }
        return out.str();
    }

    const TestSpec& spec_;
};

namespace stm {

enum class EventKind {
    tx_begin,
    tx_read,
    tx_write,
    tx_validate_begin,
    tx_validate_end,
    tx_lock_attempt,
    tx_lock_acquired,
    tx_lock_failed,
    tx_lock_released,
    tx_commit_attempt,
    tx_commit_success,
    tx_abort,
    tx_retry
};

struct Event {
    EventKind kind = EventKind::tx_begin;
    bool read_only = false;
    const void* address = nullptr;
    std::uint64_t lock_slot = 0;
    bool has_lock_slot = false;
    std::uint64_t version = 0;
    bool has_version = false;
    std::uint64_t clock = 0;
    bool has_clock = false;
    bool success = false;
    int attempt = 0;
    std::string reason;
    std::uint64_t transaction_id = 0;
    int transaction_depth = 0;
};

inline const char* event_name(EventKind kind) {
    switch (kind) {
        case EventKind::tx_begin: return "tx_begin";
        case EventKind::tx_read: return "tx_read";
        case EventKind::tx_write: return "tx_write";
        case EventKind::tx_validate_begin: return "tx_validate_begin";
        case EventKind::tx_validate_end: return "tx_validate_end";
        case EventKind::tx_lock_attempt: return "tx_lock_attempt";
        case EventKind::tx_lock_acquired: return "tx_lock_acquired";
        case EventKind::tx_lock_failed: return "tx_lock_failed";
        case EventKind::tx_lock_released: return "tx_lock_released";
        case EventKind::tx_commit_attempt: return "tx_commit_attempt";
        case EventKind::tx_commit_success: return "tx_commit_success";
        case EventKind::tx_abort: return "tx_abort";
        case EventKind::tx_retry: return "tx_retry";
    }
    return "tx_unknown";
}

inline const char* switch_location(EventKind kind) {
    switch (kind) {
        case EventKind::tx_begin: return "stm.tx_begin";
        case EventKind::tx_read: return "stm.tx_read";
        case EventKind::tx_write: return "stm.tx_write";
        case EventKind::tx_validate_begin: return "stm.tx_validate_begin";
        case EventKind::tx_validate_end: return "stm.tx_validate_end";
        case EventKind::tx_lock_attempt: return "stm.tx_lock_attempt";
        case EventKind::tx_lock_acquired: return "stm.tx_lock_acquired";
        case EventKind::tx_lock_failed: return "stm.tx_lock_failed";
        case EventKind::tx_lock_released: return "stm.tx_lock_released";
        case EventKind::tx_commit_attempt: return "stm.tx_commit_attempt";
        case EventKind::tx_commit_success: return "stm.tx_commit_success";
        case EventKind::tx_abort: return "stm.tx_abort";
        case EventKind::tx_retry: return "stm.tx_retry";
    }
    return "stm.tx_unknown";
}

inline std::string to_string(const Event& event) {
    std::ostringstream out;
    out << "stm." << event_name(event.kind);
    if (event.kind == EventKind::tx_begin) {
        out << " read_only=" << (event.read_only ? "true" : "false");
    }
    if (event.address != nullptr) {
        out << " address=" << stable_object_id(event.address);
    }
    if (event.has_lock_slot) {
        out << " lock_slot=" << event.lock_slot;
    }
    if (event.has_version) {
        out << " version=" << event.version;
    }
    if (event.has_clock) {
        out << " clock=" << event.clock;
    }
    if (event.kind == EventKind::tx_validate_end) {
        out << " success=" << (event.success ? "true" : "false");
    }
    if (event.attempt != 0) {
        out << " attempt=" << event.attempt;
    }
    if (!event.reason.empty()) {
        out << " reason=" << event.reason;
    }
    if (event.transaction_id != 0) {
        out << " tx_id=" << event.transaction_id;
    }
    if (event.transaction_depth != 0) {
        out << " tx_depth=" << event.transaction_depth;
    }
    return out.str();
}

void emit(const Event& event);
void tx_begin(bool read_only, std::uint64_t start_clock_or_version = 0);
void tx_read(const void* address, std::uint64_t lock_slot = 0, std::uint64_t version = 0);
void tx_write(const void* address, std::uint64_t lock_slot = 0);
void tx_validate_begin();
void tx_validate_end(bool success);
void tx_lock_attempt(std::uint64_t lock_slot);
void tx_lock_acquired(std::uint64_t lock_slot);
void tx_lock_failed(std::uint64_t lock_slot);
void tx_lock_released(std::uint64_t lock_slot);
void tx_commit_attempt();
void tx_commit_success(std::uint64_t commit_clock = 0);
void tx_abort(std::string reason = {});
void tx_retry(std::string reason = {}, int attempt = 0);

} // namespace stm

namespace detail {

inline StmEventRecord make_stm_event_record(
    std::size_t sequence,
    int thread_id,
    const stm::Event& event,
    std::size_t event_index = 0,
    const OperationContext* operation = nullptr
) {
    StmEventRecord record;
    record.sequence = sequence;
    record.event_index = event_index;
    record.thread_id = thread_id;
    if (operation != nullptr) {
        record.operation = *operation;
    }
    record.kind = stm::event_name(event.kind);
    record.description = stm::to_string(event);
    if (event.address != nullptr) {
        record.address_id = stable_object_id(event.address);
    }
    if (event.kind == stm::EventKind::tx_begin) {
        record.read_only = event.read_only;
        record.has_read_only = true;
    }
    record.lock_slot = event.lock_slot;
    record.has_lock_slot = event.has_lock_slot;
    record.version = event.version;
    record.has_version = event.has_version;
    record.clock = event.clock;
    record.has_clock = event.has_clock;
    if (event.kind == stm::EventKind::tx_validate_end) {
        record.success = event.success;
        record.has_success = true;
    }
    record.attempt = event.attempt;
    record.reason = event.reason;
    record.transaction_id = event.transaction_id;
    record.transaction_depth = event.transaction_depth;
    return record;
}

} // namespace detail

class Runtime {
public:
    virtual ~Runtime() = default;
    virtual void switch_point(const char* location) = 0;
    virtual void event(const std::string& description) = 0;
    virtual void operation_begin(const OperationContext& context) { (void)context; }
    virtual void operation_end(const OperationContext& context) { (void)context; }
    virtual void warning(const std::string& message) { (void)message; }
    virtual int thread_id() const { return -1; }
    virtual bool manages_locks() const { return false; }
    virtual void lock(const void*) {}
    virtual void unlock(const void*) {}
    virtual bool try_lock(const void*) { return false; }
    virtual bool try_lock_for(const void*, std::chrono::nanoseconds) { return false; }
    virtual bool owns_lock(const void*) const { return false; }
    virtual void lock_shared(const void*) {}
    virtual void unlock_shared(const void*) {}
    virtual bool try_lock_shared(const void*) { return false; }
    virtual bool try_lock_shared_for(const void*, std::chrono::nanoseconds) { return false; }
    virtual bool owns_shared_lock(const void*) const { return false; }
    virtual void wait_condition(const void*, const void*) {}
    virtual std::cv_status wait_condition_for(const void* condition, const void* lock, std::chrono::nanoseconds) {
        wait_condition(condition, lock);
        return std::cv_status::no_timeout;
    }
    virtual void notify_one_condition(const void*) {}
    virtual void notify_all_condition(const void*) {}
    virtual bool manages_atomic_waits() const { return false; }
    virtual void wait_atomic(const void*, std::string_view) {}
    virtual void notify_one_atomic(const void*) {}
    virtual void notify_all_atomic(const void*) {}
    virtual bool manages_semaphores() const { return false; }
    virtual void acquire_semaphore(const void*, std::ptrdiff_t) {}
    virtual bool try_acquire_semaphore(const void*, std::ptrdiff_t) { return false; }
    virtual bool try_acquire_semaphore_for(const void*, std::ptrdiff_t, std::chrono::nanoseconds) { return false; }
    virtual void release_semaphore(const void*, std::ptrdiff_t, std::ptrdiff_t) {}
    virtual bool manages_latches() const { return false; }
    virtual void count_down_latch(const void*, std::ptrdiff_t, std::ptrdiff_t) {}
    virtual bool try_wait_latch(const void*, std::ptrdiff_t observed_count) { return observed_count == 0; }
    virtual void wait_latch(const void*, std::ptrdiff_t) {}
    virtual bool manages_barriers() const { return false; }
    virtual void arrive_barrier(const void*, std::ptrdiff_t, bool, std::ptrdiff_t, std::ptrdiff_t, std::size_t) {}
    virtual void wait_barrier(const void*, std::size_t) {}
    virtual bool manages_parking() const { return false; }
    virtual void park(const void*) {}
    virtual void unpark(const void*) {}
    virtual void stm_event(const stm::Event& event) {
        detail::ScopedTraceTransactionMetadata metadata(event.transaction_id, event.transaction_depth);
        this->event(stm::to_string(event));
        this->switch_point(stm::switch_location(event.kind));
    }
    virtual void source_access_event(const SourceAccessEvent& event) {
        (void)event;
    }
    virtual void synchronization_event(const SynchronizationEvent& event) {
        (void)event;
    }
    virtual void memory_event(const MemoryEvent& event) {
        (void)event;
    }
};

inline thread_local Runtime* current_runtime = nullptr;

namespace detail {

inline OperationContext make_operation_context(
    int thread_id,
    std::size_t actor_index,
    const Actor& actor,
    const Operation& operation
) {
    return OperationContext{
        .thread_id = thread_id,
        .actor_index = actor_index,
        .operation_index = actor.operation_index,
        .name = actor.name,
        .group = actor.group,
        .independence_group = operation.options.independence_group,
        .non_parallel = actor.non_parallel,
        .one_shot = actor.one_shot,
        .exception_results = actor.exception_results,
        .actor_label = actor.to_string()
    };
}

class ScopedOperationContext {
public:
    explicit ScopedOperationContext(OperationContext context)
        : runtime_(current_runtime),
          context_(std::move(context)) {
        if (runtime_ != nullptr) {
            runtime_->operation_begin(context_);
            active_ = true;
        }
    }

    ScopedOperationContext(const ScopedOperationContext&) = delete;
    ScopedOperationContext& operator=(const ScopedOperationContext&) = delete;

    ~ScopedOperationContext() {
        close();
    }

    void close() {
        if (active_) {
            runtime_->operation_end(context_);
            active_ = false;
        }
    }

private:
    Runtime* runtime_ = nullptr;
    OperationContext context_;
    bool active_ = false;
};

} // namespace detail

class WarningSink {
public:
    virtual ~WarningSink() = default;
    virtual void warning(const std::string& message) = 0;
};

inline thread_local WarningSink* current_warning_sink = nullptr;

class ScopedRuntime {
public:
    explicit ScopedRuntime(Runtime* runtime) : previous_(current_runtime) {
        current_runtime = runtime;
    }

    ~ScopedRuntime() {
        current_runtime = previous_;
    }

    ScopedRuntime(const ScopedRuntime&) = delete;
    ScopedRuntime& operator=(const ScopedRuntime&) = delete;

private:
    Runtime* previous_;
};

class ScopedWarningSink {
public:
    explicit ScopedWarningSink(WarningSink* sink) : previous_(current_warning_sink) {
        current_warning_sink = sink;
    }

    ~ScopedWarningSink() {
        current_warning_sink = previous_;
    }

    ScopedWarningSink(const ScopedWarningSink&) = delete;
    ScopedWarningSink& operator=(const ScopedWarningSink&) = delete;

private:
    WarningSink* previous_;
};

namespace detail {

class VectorWarningSink final : public WarningSink {
public:
    VectorWarningSink(std::vector<std::string>& warnings, std::mutex& mutex)
        : warnings_(warnings), mutex_(mutex) {}

    void warning(const std::string& message) override {
        std::lock_guard lock(mutex_);
        append_unique_warning(warnings_, message);
    }

private:
    std::vector<std::string>& warnings_;
    std::mutex& mutex_;
};

} // namespace detail

inline void emit_warning(const std::string& message) {
    if (current_runtime) {
        current_runtime->warning(message);
    }
    if (current_warning_sink) {
        current_warning_sink->warning(message);
    }
}

inline std::string non_seq_cst_memory_order_warning(std::memory_order order, const std::string& operation) {
    return "non-seq_cst memory order " + std::string(memory_order_name(order)) +
        " observed in " + operation +
        "; lincheck records the order label but currently explores wrapper behavior as sequentially consistent";
}

inline void warn_if_non_seq_cst(std::memory_order order, const std::string& operation) {
    if (order != std::memory_order_seq_cst) {
        emit_warning(non_seq_cst_memory_order_warning(order, operation));
    }
}

inline void switch_point(const char* location = "manual") {
    if (current_runtime) {
        current_runtime->switch_point(location);
    }
}

inline void switch_point(const char* location, const SourceLocation& source) {
    if (current_runtime) {
        const auto full_location = std::string(location) + " @ " + source.to_string();
        current_runtime->switch_point(full_location.c_str());
    }
}

inline void switch_point(const SourceLocation& source) {
    switch_point("manual", source);
}

inline void yield(const char* location = "yield") {
    switch_point(location);
    if (!current_runtime) {
        std::this_thread::yield();
    }
}

inline void yield(const SourceLocation& source) {
    switch_point("yield", source);
    if (!current_runtime) {
        std::this_thread::yield();
    }
}

inline void trace_event(const std::string& description) {
    if (current_runtime) {
        current_runtime->event(description);
    }
}

inline void trace_event(const std::string& description, const SourceLocation& source) {
    trace_event(description + " @ " + source.to_string());
}

inline void emit_memory_event(const MemoryEvent& event) {
    if (current_runtime) {
        current_runtime->memory_event(event);
    }
}

inline void emit_source_access_event(const SourceAccessEvent& event) {
    if (current_runtime) {
        current_runtime->source_access_event(event);
    }
}

inline void emit_synchronization_event(const SynchronizationEvent& event) {
    if (current_runtime) {
        current_runtime->synchronization_event(event);
    }
}

inline void publish_synchronization_event(
    SynchronizationEventKind kind,
    const void* object,
    const void* related_object = nullptr,
    std::string detail = {}
) {
    SynchronizationEvent event;
    event.kind = kind;
    event.object = object;
    event.related_object = related_object;
    event.detail = std::move(detail);
    emit_synchronization_event(event);
}

inline void publish_synchronization_event(
    SynchronizationEventKind kind,
    const void* object,
    bool success,
    std::string detail = {}
) {
    SynchronizationEvent event;
    event.kind = kind;
    event.object = object;
    event.success = success;
    event.has_success = true;
    event.detail = std::move(detail);
    emit_synchronization_event(event);
}

namespace detail {

inline void trace_event_maybe_source(const std::string& description, const SourceLocation* source) {
    if (source) {
        trace_event(description, *source);
    } else {
        trace_event(description);
    }
}

inline void switch_point_maybe_source(const char* location, const SourceLocation* source) {
    if (source) {
        switch_point(location, *source);
    } else {
        switch_point(location);
    }
}

inline void trace_call_transition(const std::string& transition, const std::string& name, const SourceLocation* source) {
    const std::string event = "call." + transition;
    trace_event_maybe_source(event + " " + name, source);
    switch_point_maybe_source(event.c_str(), source);
}

template <typename T>
void trace_call_return(const std::string& name, const T& value, const SourceLocation* source) {
    const std::string event = "call.end";
    trace_event_maybe_source(event + " " + name + " -> " + trace_value_string(value), source);
    switch_point_maybe_source(event.c_str(), source);
}

template <typename Fn>
decltype(auto) invoke_call_body(const std::string& name, Fn&& fn, const SourceLocation* source) {
    try {
        return std::invoke(std::forward<Fn>(fn));
    } catch (...) {
        trace_call_transition("throw", name, source);
        throw;
    }
}

template <typename Fn>
decltype(auto) call_impl(std::string name, Fn&& fn, const SourceLocation* source) {
    using Result = std::invoke_result_t<Fn&&>;

    trace_call_transition("begin", name, source);
    if constexpr (std::is_void_v<Result>) {
        invoke_call_body(name, std::forward<Fn>(fn), source);
        trace_call_transition("end", name, source);
        return;
    } else if constexpr (std::is_lvalue_reference_v<Result>) {
        Result result = invoke_call_body(name, std::forward<Fn>(fn), source);
        trace_call_return(name, result, source);
        return result;
    } else if constexpr (std::is_rvalue_reference_v<Result>) {
        Result result = invoke_call_body(name, std::forward<Fn>(fn), source);
        trace_call_return(name, result, source);
        return std::forward<std::remove_reference_t<Result>>(result);
    } else {
        Result result = invoke_call_body(name, std::forward<Fn>(fn), source);
        trace_call_return(name, result, source);
        return result;
    }
}

} // namespace detail

template <typename Fn>
decltype(auto) call(std::string name, Fn&& fn) {
    return detail::call_impl(std::move(name), std::forward<Fn>(fn), nullptr);
}

template <typename Fn>
decltype(auto) call(std::string name, const SourceLocation& source, Fn&& fn) {
    return detail::call_impl(std::move(name), std::forward<Fn>(fn), &source);
}

inline void atomic_thread_fence(std::memory_order order = std::memory_order_seq_cst) {
    validate_known_memory_order(order, "atomic_thread_fence");
    warn_if_non_seq_cst(order, "atomic_thread_fence");
    trace_event("atomic_thread_fence order=" + std::string(memory_order_name(order)));
    MemoryEvent event;
    event.kind = MemoryEventKind::atomic_thread_fence;
    event.success_order = order;
    emit_memory_event(event);
    std::atomic_thread_fence(order);
    switch_point("atomic_thread_fence");
}

inline void atomic_signal_fence(std::memory_order order = std::memory_order_seq_cst) {
    validate_known_memory_order(order, "atomic_signal_fence");
    warn_if_non_seq_cst(order, "atomic_signal_fence");
    trace_event("atomic_signal_fence order=" + std::string(memory_order_name(order)));
    MemoryEvent event;
    event.kind = MemoryEventKind::atomic_signal_fence;
    event.success_order = order;
    emit_memory_event(event);
    std::atomic_signal_fence(order);
    switch_point("atomic_signal_fence");
}

inline void atomic_thread_fence(const SourceLocation& source, std::memory_order order = std::memory_order_seq_cst) {
    validate_known_memory_order(order, "atomic_thread_fence");
    warn_if_non_seq_cst(order, "atomic_thread_fence");
    trace_event("atomic_thread_fence order=" + std::string(memory_order_name(order)), source);
    MemoryEvent event;
    event.kind = MemoryEventKind::atomic_thread_fence;
    event.success_order = order;
    event.source = source;
    event.has_source = true;
    emit_memory_event(event);
    std::atomic_thread_fence(order);
    switch_point("atomic_thread_fence", source);
}

inline void atomic_signal_fence(const SourceLocation& source, std::memory_order order = std::memory_order_seq_cst) {
    validate_known_memory_order(order, "atomic_signal_fence");
    warn_if_non_seq_cst(order, "atomic_signal_fence");
    trace_event("atomic_signal_fence order=" + std::string(memory_order_name(order)), source);
    MemoryEvent event;
    event.kind = MemoryEventKind::atomic_signal_fence;
    event.success_order = order;
    event.source = source;
    event.has_source = true;
    emit_memory_event(event);
    std::atomic_signal_fence(order);
    switch_point("atomic_signal_fence", source);
}

template <typename T>
T read(const T& value, const SourceLocation& source) {
    T snapshot = value;
    SourceAccessEvent event;
    event.kind = SourceAccessKind::read;
    event.object = std::addressof(value);
    event.source = source;
    if constexpr (std::is_constructible_v<Value, std::decay_t<T>>) {
        event.value = Value(snapshot);
        event.has_value = true;
    }
    emit_source_access_event(event);
    trace_event(
        "source.read " + stable_object_id(std::addressof(value)) +
        " " + stable_location_id(source) +
        " -> " + trace_value_string(snapshot),
        source
    );
    switch_point("source.read", source);
    return snapshot;
}

template <typename T, typename U>
void write(T& target, U&& value, const SourceLocation& source) {
    SourceAccessEvent event;
    event.kind = SourceAccessKind::write;
    event.object = std::addressof(target);
    event.source = source;
    if constexpr (std::is_constructible_v<Value, std::decay_t<U>>) {
        event.value = Value(value);
        event.has_value = true;
    }
    emit_source_access_event(event);
    trace_event(
        "source.write " + stable_object_id(std::addressof(target)) +
        " " + stable_location_id(source) +
        " " + trace_value_string(value),
        source
    );
    target = std::forward<U>(value);
    switch_point("source.write", source);
}

namespace stm {

struct TransactionContext {
    std::uint64_t next_id = 1;
    std::uint64_t current_id = 0;
    std::uint64_t last_aborted_id = 0;
    int depth = 0;
    int last_aborted_depth = 0;
};

inline thread_local TransactionContext transaction_context;

inline void attach_current_transaction(Event& event) {
    if (transaction_context.current_id == 0) return;
    event.transaction_id = transaction_context.current_id;
    event.transaction_depth = transaction_context.depth;
}

inline void attach_last_aborted_transaction(Event& event) {
    if (transaction_context.last_aborted_id == 0) return;
    event.transaction_id = transaction_context.last_aborted_id;
    event.transaction_depth = transaction_context.last_aborted_depth;
}

inline void emit(const Event& event) {
    if (current_runtime) {
        current_runtime->stm_event(event);
    }
}

inline void tx_begin(bool read_only, std::uint64_t start_clock_or_version) {
    transaction_context.last_aborted_id = 0;
    transaction_context.last_aborted_depth = 0;
    if (transaction_context.current_id == 0) {
        transaction_context.current_id = transaction_context.next_id++;
        transaction_context.depth = 1;
    } else {
        ++transaction_context.depth;
    }

    Event event;
    event.kind = EventKind::tx_begin;
    event.read_only = read_only;
    event.clock = start_clock_or_version;
    event.has_clock = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_read(const void* address, std::uint64_t lock_slot, std::uint64_t version) {
    Event event;
    event.kind = EventKind::tx_read;
    event.address = address;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    event.version = version;
    event.has_version = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_write(const void* address, std::uint64_t lock_slot) {
    Event event;
    event.kind = EventKind::tx_write;
    event.address = address;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_validate_begin() {
    Event event;
    event.kind = EventKind::tx_validate_begin;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_validate_end(bool success) {
    Event event;
    event.kind = EventKind::tx_validate_end;
    event.success = success;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_lock_attempt(std::uint64_t lock_slot) {
    Event event;
    event.kind = EventKind::tx_lock_attempt;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_lock_acquired(std::uint64_t lock_slot) {
    Event event;
    event.kind = EventKind::tx_lock_acquired;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_lock_failed(std::uint64_t lock_slot) {
    Event event;
    event.kind = EventKind::tx_lock_failed;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_lock_released(std::uint64_t lock_slot) {
    Event event;
    event.kind = EventKind::tx_lock_released;
    event.lock_slot = lock_slot;
    event.has_lock_slot = true;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_commit_attempt() {
    Event event;
    event.kind = EventKind::tx_commit_attempt;
    attach_current_transaction(event);
    emit(event);
}

inline void tx_commit_success(std::uint64_t commit_clock) {
    Event event;
    event.kind = EventKind::tx_commit_success;
    event.clock = commit_clock;
    event.has_clock = true;
    attach_current_transaction(event);
    emit(event);
    if (transaction_context.depth > 1) {
        --transaction_context.depth;
    } else {
        transaction_context.current_id = 0;
        transaction_context.depth = 0;
    }
}

inline void tx_abort(std::string reason) {
    Event event;
    event.kind = EventKind::tx_abort;
    event.reason = std::move(reason);
    attach_current_transaction(event);
    emit(event);
    transaction_context.last_aborted_id = transaction_context.current_id;
    transaction_context.last_aborted_depth = transaction_context.depth;
    transaction_context.current_id = 0;
    transaction_context.depth = 0;
}

inline void tx_retry(std::string reason, int attempt) {
    Event event;
    event.kind = EventKind::tx_retry;
    event.attempt = attempt;
    event.reason = std::move(reason);
    attach_current_transaction(event);
    if (event.transaction_id == 0) {
        attach_last_aborted_transaction(event);
    }
    emit(event);
    transaction_context.last_aborted_id = 0;
    transaction_context.last_aborted_depth = 0;
}

} // namespace stm

template <typename T>
class atomic {
public:
    atomic() = default;
    explicit atomic(T value) : value_(value) {}

    operator T() const {
        return load();
    }

    T operator=(T value) {
        store(value);
        return value;
    }

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        validate_load_memory_order(order, "atomic.load");
        const auto value = value_.load(order);
        warn_if_non_seq_cst(order, "atomic.load");
        trace_event(
            "atomic.load order=" + std::string(memory_order_name(order)) +
            " -> " + Value(value).to_string() +
            " object=" + identity_string()
        );
        publish_observed_event(MemoryEventKind::atomic_load, order, value);
        switch_point("atomic.load");
        if (!current_runtime) std::this_thread::yield();
        return value;
    }

    void store(T value, std::memory_order order = std::memory_order_seq_cst) {
        validate_store_memory_order(order, "atomic.store");
        warn_if_non_seq_cst(order, "atomic.store");
        trace_event(
            "atomic.store order=" + std::string(memory_order_name(order)) +
            " " + Value(value).to_string() +
            " object=" + identity_string()
        );
        publish_operand_event(MemoryEventKind::atomic_store, order, value);
        value_.store(value, order);
        switch_point("atomic.store");
    }

    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) {
        validate_known_memory_order(order, "atomic.exchange");
        const auto old = value_.exchange(value, order);
        warn_if_non_seq_cst(order, "atomic.exchange");
        trace_event(
            "atomic.exchange order=" + std::string(memory_order_name(order)) +
            " " + Value(value).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_exchange, order, value, old);
        switch_point("atomic.exchange");
        return old;
    }

    T fetch_add(T delta, std::memory_order order = std::memory_order_seq_cst) {
        validate_known_memory_order(order, "atomic.fetch_add");
        const auto old = value_.fetch_add(delta, order);
        warn_if_non_seq_cst(order, "atomic.fetch_add");
        trace_event(
            "atomic.fetch_add order=" + std::string(memory_order_name(order)) +
            " " + Value(delta).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_add, order, delta, old);
        switch_point("atomic.fetch_add");
        return old;
    }

    T fetch_sub(T delta, std::memory_order order = std::memory_order_seq_cst) {
        validate_known_memory_order(order, "atomic.fetch_sub");
        const auto old = value_.fetch_sub(delta, order);
        warn_if_non_seq_cst(order, "atomic.fetch_sub");
        trace_event(
            "atomic.fetch_sub order=" + std::string(memory_order_name(order)) +
            " " + Value(delta).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_sub, order, delta, old);
        switch_point("atomic.fetch_sub");
        return old;
    }

    T fetch_and(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic.fetch_and");
        const auto old = value_.fetch_and(mask, order);
        warn_if_non_seq_cst(order, "atomic.fetch_and");
        trace_event(
            "atomic.fetch_and order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_and, order, mask, old);
        switch_point("atomic.fetch_and");
        return old;
    }

    T fetch_or(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic.fetch_or");
        const auto old = value_.fetch_or(mask, order);
        warn_if_non_seq_cst(order, "atomic.fetch_or");
        trace_event(
            "atomic.fetch_or order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_or, order, mask, old);
        switch_point("atomic.fetch_or");
        return old;
    }

    T fetch_xor(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic.fetch_xor");
        const auto old = value_.fetch_xor(mask, order);
        warn_if_non_seq_cst(order, "atomic.fetch_xor");
        trace_event(
            "atomic.fetch_xor order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_xor, order, mask, old);
        switch_point("atomic.fetch_xor");
        return old;
    }

    T operator++()
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(static_cast<T>(1)) + static_cast<T>(1);
    }

    T operator++(int)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(static_cast<T>(1));
    }

    T operator--()
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(static_cast<T>(1)) - static_cast<T>(1);
    }

    T operator--(int)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(static_cast<T>(1));
    }

    T operator+=(T delta)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(delta) + delta;
    }

    T operator-=(T delta)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(delta) - delta;
    }

    T operator&=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_and(mask) & mask;
    }

    T operator|=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_or(mask) | mask;
    }

    T operator^=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_xor(mask) ^ mask;
    }

    bool compare_exchange_strong(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) {
        return compare_exchange_strong(expected, desired, order, compare_exchange_failure_order(order));
    }

    bool compare_exchange_strong(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) {
        validate_compare_exchange_memory_orders(success, failure, "atomic.compare_exchange_strong");
        const auto before = expected;
        const bool ok = value_.compare_exchange_strong(expected, desired, success, failure);
        warn_if_non_seq_cst(success, "atomic.compare_exchange_strong success");
        warn_if_non_seq_cst(failure, "atomic.compare_exchange_strong failure");
        trace_compare_exchange("atomic.compare_exchange_strong", before, expected, desired, success, failure, ok);
        publish_compare_exchange_event(
            MemoryEventKind::atomic_compare_exchange_strong,
            success,
            failure,
            before,
            desired,
            expected,
            ok
        );
        switch_point("atomic.compare_exchange_strong");
        return ok;
    }

    bool compare_exchange_weak(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) {
        return compare_exchange_weak(expected, desired, order, compare_exchange_failure_order(order));
    }

    bool compare_exchange_weak(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) {
        validate_compare_exchange_memory_orders(success, failure, "atomic.compare_exchange_weak");
        const auto before = expected;
        const bool ok = value_.compare_exchange_weak(expected, desired, success, failure);
        warn_if_non_seq_cst(success, "atomic.compare_exchange_weak success");
        warn_if_non_seq_cst(failure, "atomic.compare_exchange_weak failure");
        trace_compare_exchange("atomic.compare_exchange_weak", before, expected, desired, success, failure, ok);
        publish_compare_exchange_event(
            MemoryEventKind::atomic_compare_exchange_weak,
            success,
            failure,
            before,
            desired,
            expected,
            ok
        );
        switch_point("atomic.compare_exchange_weak");
        return ok;
    }

    void wait(T old, std::memory_order order = std::memory_order_seq_cst) const {
        validate_wait_memory_order(order, "atomic.wait");
        warn_if_non_seq_cst(order, "atomic.wait");
        const auto expected = Value(old).to_string();
        publish_synchronization_event(SynchronizationEventKind::atomic_wait, this, nullptr, "expected=" + expected);
        trace_event(
            "atomic.wait order=" + std::string(memory_order_name(order)) +
            " expected=" + expected +
            " object=" + identity_string()
        );
        publish_expected_event(MemoryEventKind::atomic_wait, order, old);

        if (current_runtime && current_runtime->manages_atomic_waits()) {
            while (value_.load(order) == old) {
                current_runtime->wait_atomic(this, expected);
            }
            publish_synchronization_event(SynchronizationEventKind::atomic_wake, this, nullptr, "expected=" + expected);
            current_runtime->switch_point("atomic.wait");
            return;
        }

        value_.wait(old, order);
        publish_synchronization_event(SynchronizationEventKind::atomic_wake, this, nullptr, "expected=" + expected);
        switch_point("atomic.wait");
    }

    void notify_one() {
        trace_event("atomic.notify_one object=" + identity_string());
        publish_simple_event(MemoryEventKind::atomic_notify_one);
        publish_synchronization_event(SynchronizationEventKind::atomic_notify_one, this);
        if (current_runtime && current_runtime->manages_atomic_waits()) {
            current_runtime->notify_one_atomic(this);
            current_runtime->switch_point("atomic.notify_one");
            return;
        }

        value_.notify_one();
        switch_point("atomic.notify_one");
    }

    void notify_all() {
        trace_event("atomic.notify_all object=" + identity_string());
        publish_simple_event(MemoryEventKind::atomic_notify_all);
        publish_synchronization_event(SynchronizationEventKind::atomic_notify_all, this);
        if (current_runtime && current_runtime->manages_atomic_waits()) {
            current_runtime->notify_all_atomic(this);
            current_runtime->switch_point("atomic.notify_all");
            return;
        }

        value_.notify_all();
        switch_point("atomic.notify_all");
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    void publish_memory_event(MemoryEvent event) const {
        event.object = this;
        emit_memory_event(event);
    }

    void publish_simple_event(MemoryEventKind kind) const {
        MemoryEvent event;
        event.kind = kind;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_operand_event(MemoryEventKind kind, std::memory_order order, const U& operand) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.operand = Value(operand);
        event.has_operand = true;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_expected_event(MemoryEventKind kind, std::memory_order order, const U& expected) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.expected = Value(expected);
        event.has_expected = true;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_observed_event(MemoryEventKind kind, std::memory_order order, const U& observed) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.observed = Value(observed);
        event.has_observed = true;
        publish_memory_event(std::move(event));
    }

    template <typename U, typename V>
    void publish_operand_observed_event(
        MemoryEventKind kind,
        std::memory_order order,
        const U& operand,
        const V& observed
    ) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.operand = Value(operand);
        event.has_operand = true;
        event.observed = Value(observed);
        event.has_observed = true;
        publish_memory_event(std::move(event));
    }

    template <typename U, typename V, typename W>
    void publish_compare_exchange_event(
        MemoryEventKind kind,
        std::memory_order success_order,
        std::memory_order failure_order,
        const U& expected_before,
        const V& desired,
        const W& expected_after,
        bool ok
    ) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = success_order;
        event.failure_order = failure_order;
        event.has_failure_order = true;
        event.expected = Value(expected_before);
        event.has_expected = true;
        event.operand = Value(desired);
        event.has_operand = true;
        event.observed = Value(expected_after);
        event.has_observed = true;
        event.success = ok;
        event.has_success = true;
        publish_memory_event(std::move(event));
    }

    void trace_compare_exchange(
        const char* name,
        T expected_before,
        T expected_after,
        T desired,
        std::memory_order success,
        std::memory_order failure,
        bool ok
    ) {
        trace_event(
            std::string(name) +
            " success_order=" + memory_order_name(success) +
            " failure_order=" + memory_order_name(failure) +
            " expected=" + Value(expected_before).to_string() +
            " desired=" + Value(desired).to_string() +
            " expected_after=" + Value(expected_after).to_string() +
            " -> " + (ok ? "true" : "false") +
            " object=" + identity_string()
        );
    }

    mutable std::atomic<T> value_{};
};

template <typename T>
class atomic_ref {
public:
    static constexpr bool is_always_lock_free = std::atomic_ref<T>::is_always_lock_free;
    static constexpr std::size_t required_alignment = std::atomic_ref<T>::required_alignment;

    explicit atomic_ref(T& object) noexcept : object_(std::addressof(object)), ref_(object) {}
    atomic_ref(const atomic_ref&) noexcept = default;
    atomic_ref& operator=(const atomic_ref&) = delete;

    bool is_lock_free() const noexcept {
        return ref_.is_lock_free();
    }

    operator T() const {
        return load();
    }

    T operator=(T value) {
        store(value);
        return value;
    }

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        validate_load_memory_order(order, "atomic_ref.load");
        const auto value = ref_.load(order);
        warn_if_non_seq_cst(order, "atomic_ref.load");
        trace_event(
            "atomic_ref.load order=" + std::string(memory_order_name(order)) +
            " -> " + Value(value).to_string() +
            " object=" + identity_string()
        );
        publish_observed_event(MemoryEventKind::atomic_load, order, value);
        switch_point("atomic_ref.load");
        if (!current_runtime) std::this_thread::yield();
        return value;
    }

    void store(T value, std::memory_order order = std::memory_order_seq_cst) {
        validate_store_memory_order(order, "atomic_ref.store");
        warn_if_non_seq_cst(order, "atomic_ref.store");
        trace_event(
            "atomic_ref.store order=" + std::string(memory_order_name(order)) +
            " " + Value(value).to_string() +
            " object=" + identity_string()
        );
        publish_operand_event(MemoryEventKind::atomic_store, order, value);
        ref_.store(value, order);
        switch_point("atomic_ref.store");
    }

    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) {
        validate_known_memory_order(order, "atomic_ref.exchange");
        const auto old = ref_.exchange(value, order);
        warn_if_non_seq_cst(order, "atomic_ref.exchange");
        trace_event(
            "atomic_ref.exchange order=" + std::string(memory_order_name(order)) +
            " " + Value(value).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_exchange, order, value, old);
        switch_point("atomic_ref.exchange");
        return old;
    }

    T fetch_add(T delta, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic_ref.fetch_add");
        const auto old = ref_.fetch_add(delta, order);
        warn_if_non_seq_cst(order, "atomic_ref.fetch_add");
        trace_event(
            "atomic_ref.fetch_add order=" + std::string(memory_order_name(order)) +
            " " + Value(delta).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_add, order, delta, old);
        switch_point("atomic_ref.fetch_add");
        return old;
    }

    T fetch_sub(T delta, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic_ref.fetch_sub");
        const auto old = ref_.fetch_sub(delta, order);
        warn_if_non_seq_cst(order, "atomic_ref.fetch_sub");
        trace_event(
            "atomic_ref.fetch_sub order=" + std::string(memory_order_name(order)) +
            " " + Value(delta).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_sub, order, delta, old);
        switch_point("atomic_ref.fetch_sub");
        return old;
    }

    T fetch_and(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic_ref.fetch_and");
        const auto old = ref_.fetch_and(mask, order);
        warn_if_non_seq_cst(order, "atomic_ref.fetch_and");
        trace_event(
            "atomic_ref.fetch_and order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_and, order, mask, old);
        switch_point("atomic_ref.fetch_and");
        return old;
    }

    T fetch_or(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic_ref.fetch_or");
        const auto old = ref_.fetch_or(mask, order);
        warn_if_non_seq_cst(order, "atomic_ref.fetch_or");
        trace_event(
            "atomic_ref.fetch_or order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_or, order, mask, old);
        switch_point("atomic_ref.fetch_or");
        return old;
    }

    T fetch_xor(T mask, std::memory_order order = std::memory_order_seq_cst)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        validate_known_memory_order(order, "atomic_ref.fetch_xor");
        const auto old = ref_.fetch_xor(mask, order);
        warn_if_non_seq_cst(order, "atomic_ref.fetch_xor");
        trace_event(
            "atomic_ref.fetch_xor order=" + std::string(memory_order_name(order)) +
            " " + Value(mask).to_string() + " -> " + Value(old).to_string() +
            " object=" + identity_string()
        );
        publish_operand_observed_event(MemoryEventKind::atomic_fetch_xor, order, mask, old);
        switch_point("atomic_ref.fetch_xor");
        return old;
    }

    T operator++()
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(static_cast<T>(1)) + static_cast<T>(1);
    }

    T operator++(int)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(static_cast<T>(1));
    }

    T operator--()
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(static_cast<T>(1)) - static_cast<T>(1);
    }

    T operator--(int)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(static_cast<T>(1));
    }

    T operator+=(T delta)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_add(delta) + delta;
    }

    T operator-=(T delta)
        requires (std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_sub(delta) - delta;
    }

    T operator&=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_and(mask) & mask;
    }

    T operator|=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_or(mask) | mask;
    }

    T operator^=(T mask)
        requires (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return fetch_xor(mask) ^ mask;
    }

    bool compare_exchange_strong(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) {
        return compare_exchange_strong(expected, desired, order, compare_exchange_failure_order(order));
    }

    bool compare_exchange_strong(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) {
        validate_compare_exchange_memory_orders(success, failure, "atomic_ref.compare_exchange_strong");
        const auto before = expected;
        const bool ok = ref_.compare_exchange_strong(expected, desired, success, failure);
        warn_if_non_seq_cst(success, "atomic_ref.compare_exchange_strong success");
        warn_if_non_seq_cst(failure, "atomic_ref.compare_exchange_strong failure");
        trace_compare_exchange("atomic_ref.compare_exchange_strong", before, expected, desired, success, failure, ok);
        publish_compare_exchange_event(
            MemoryEventKind::atomic_compare_exchange_strong,
            success,
            failure,
            before,
            desired,
            expected,
            ok
        );
        switch_point("atomic_ref.compare_exchange_strong");
        return ok;
    }

    bool compare_exchange_weak(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) {
        return compare_exchange_weak(expected, desired, order, compare_exchange_failure_order(order));
    }

    bool compare_exchange_weak(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) {
        validate_compare_exchange_memory_orders(success, failure, "atomic_ref.compare_exchange_weak");
        const auto before = expected;
        const bool ok = ref_.compare_exchange_weak(expected, desired, success, failure);
        warn_if_non_seq_cst(success, "atomic_ref.compare_exchange_weak success");
        warn_if_non_seq_cst(failure, "atomic_ref.compare_exchange_weak failure");
        trace_compare_exchange("atomic_ref.compare_exchange_weak", before, expected, desired, success, failure, ok);
        publish_compare_exchange_event(
            MemoryEventKind::atomic_compare_exchange_weak,
            success,
            failure,
            before,
            desired,
            expected,
            ok
        );
        switch_point("atomic_ref.compare_exchange_weak");
        return ok;
    }

    void wait(T old, std::memory_order order = std::memory_order_seq_cst) const {
        validate_wait_memory_order(order, "atomic_ref.wait");
        warn_if_non_seq_cst(order, "atomic_ref.wait");
        const auto expected = Value(old).to_string();
        publish_synchronization_event(SynchronizationEventKind::atomic_wait, object_address(), nullptr, "expected=" + expected);
        trace_event(
            "atomic_ref.wait order=" + std::string(memory_order_name(order)) +
            " expected=" + expected +
            " object=" + identity_string()
        );
        publish_expected_event(MemoryEventKind::atomic_wait, order, old);

        if (current_runtime && current_runtime->manages_atomic_waits()) {
            while (ref_.load(order) == old) {
                current_runtime->wait_atomic(object_address(), expected);
            }
            publish_synchronization_event(SynchronizationEventKind::atomic_wake, object_address(), nullptr, "expected=" + expected);
            current_runtime->switch_point("atomic_ref.wait");
            return;
        }

        ref_.wait(old, order);
        publish_synchronization_event(SynchronizationEventKind::atomic_wake, object_address(), nullptr, "expected=" + expected);
        switch_point("atomic_ref.wait");
    }

    void notify_one() const {
        trace_event("atomic_ref.notify_one object=" + identity_string());
        publish_simple_event(MemoryEventKind::atomic_notify_one);
        publish_synchronization_event(SynchronizationEventKind::atomic_notify_one, object_address());
        if (current_runtime && current_runtime->manages_atomic_waits()) {
            current_runtime->notify_one_atomic(object_address());
            current_runtime->switch_point("atomic_ref.notify_one");
            return;
        }

        ref_.notify_one();
        switch_point("atomic_ref.notify_one");
    }

    void notify_all() const {
        trace_event("atomic_ref.notify_all object=" + identity_string());
        publish_simple_event(MemoryEventKind::atomic_notify_all);
        publish_synchronization_event(SynchronizationEventKind::atomic_notify_all, object_address());
        if (current_runtime && current_runtime->manages_atomic_waits()) {
            current_runtime->notify_all_atomic(object_address());
            current_runtime->switch_point("atomic_ref.notify_all");
            return;
        }

        ref_.notify_all();
        switch_point("atomic_ref.notify_all");
    }

private:
    const void* object_address() const {
        return static_cast<const void*>(object_);
    }

    std::string identity_string() const {
        return stable_object_id(object_address());
    }

    void publish_memory_event(MemoryEvent event) const {
        event.object = object_address();
        emit_memory_event(event);
    }

    void publish_simple_event(MemoryEventKind kind) const {
        MemoryEvent event;
        event.kind = kind;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_operand_event(MemoryEventKind kind, std::memory_order order, const U& operand) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.operand = Value(operand);
        event.has_operand = true;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_expected_event(MemoryEventKind kind, std::memory_order order, const U& expected) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.expected = Value(expected);
        event.has_expected = true;
        publish_memory_event(std::move(event));
    }

    template <typename U>
    void publish_observed_event(MemoryEventKind kind, std::memory_order order, const U& observed) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.observed = Value(observed);
        event.has_observed = true;
        publish_memory_event(std::move(event));
    }

    template <typename U, typename V>
    void publish_operand_observed_event(
        MemoryEventKind kind,
        std::memory_order order,
        const U& operand,
        const V& observed
    ) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = order;
        event.operand = Value(operand);
        event.has_operand = true;
        event.observed = Value(observed);
        event.has_observed = true;
        publish_memory_event(std::move(event));
    }

    template <typename U, typename V, typename W>
    void publish_compare_exchange_event(
        MemoryEventKind kind,
        std::memory_order success_order,
        std::memory_order failure_order,
        const U& expected_before,
        const V& desired,
        const W& expected_after,
        bool ok
    ) const {
        MemoryEvent event;
        event.kind = kind;
        event.success_order = success_order;
        event.failure_order = failure_order;
        event.has_failure_order = true;
        event.expected = Value(expected_before);
        event.has_expected = true;
        event.operand = Value(desired);
        event.has_operand = true;
        event.observed = Value(expected_after);
        event.has_observed = true;
        event.success = ok;
        event.has_success = true;
        publish_memory_event(std::move(event));
    }

    void trace_compare_exchange(
        const char* name,
        T expected_before,
        T expected_after,
        T desired,
        std::memory_order success,
        std::memory_order failure,
        bool ok
    ) {
        trace_event(
            std::string(name) +
            " success_order=" + memory_order_name(success) +
            " failure_order=" + memory_order_name(failure) +
            " expected=" + Value(expected_before).to_string() +
            " desired=" + Value(desired).to_string() +
            " expected_after=" + Value(expected_after).to_string() +
            " -> " + (ok ? "true" : "false") +
            " object=" + identity_string()
        );
    }

    T* object_ = nullptr;
    std::atomic_ref<T> ref_;
};

template <typename T>
class var {
public:
    var() = default;
    explicit var(T value) : value_(std::move(value)) {}

    operator T() const {
        return get();
    }

    var& operator=(T value) {
        set(std::move(value));
        return *this;
    }

    T get() const {
        T value;
        {
            std::lock_guard lock(mutex_);
            value = value_;
        }
        trace_event("var.read -> " + Value(value).to_string() + " object=" + identity_string());
        switch_point("var.read");
        return value;
    }

    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            trace_event("var.write " + Value(value).to_string() + " object=" + identity_string());
            value_ = std::move(value);
        }
        switch_point("var.write");
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    mutable std::mutex mutex_;
    T value_{};
};

class mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("mutex.lock object=" + identity_string());
            current_runtime->switch_point("mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this);
            current_runtime->switch_point("mutex.lock.after");
            return;
        }

        trace_event("mutex.lock object=" + identity_string());
        switch_point("mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this);
        switch_point("mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("mutex.unlock");
            return;
        }

        trace_event("mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok);
            current_runtime->switch_point("mutex.try_lock");
            return ok;
        }

        trace_event("mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok);
        switch_point("mutex.try_lock");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::mutex mutex_;
};

class recursive_mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "recursive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_mutex.lock object=" + identity_string());
            current_runtime->switch_point("recursive_mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "recursive");
            current_runtime->switch_point("recursive_mutex.lock.after");
            return;
        }

        trace_event("recursive_mutex.lock object=" + identity_string());
        switch_point("recursive_mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "recursive");
        switch_point("recursive_mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "recursive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("recursive_mutex.unlock");
            return;
        }

        trace_event("recursive_mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("recursive_mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "recursive");
            current_runtime->switch_point("recursive_mutex.try_lock");
            return ok;
        }

        trace_event("recursive_mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "recursive");
        switch_point("recursive_mutex.try_lock");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::recursive_mutex mutex_;
};

class timed_mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "timed");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("timed_mutex.lock object=" + identity_string());
            current_runtime->switch_point("timed_mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "timed");
            current_runtime->switch_point("timed_mutex.lock.after");
            return;
        }

        trace_event("timed_mutex.lock object=" + identity_string());
        switch_point("timed_mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "timed");
        switch_point("timed_mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "timed");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("timed_mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("timed_mutex.unlock");
            return;
        }

        trace_event("timed_mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("timed_mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("timed_mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed");
            current_runtime->switch_point("timed_mutex.try_lock");
            return ok;
        }

        trace_event("timed_mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed");
        switch_point("timed_mutex.try_lock");
        return ok;
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("timed_mutex.try_lock_for object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, nanos);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed try_lock_for");
            current_runtime->switch_point("timed_mutex.try_lock_for");
            return ok;
        }

        trace_event("timed_mutex.try_lock_for object=" + identity_string());
        const bool ok = mutex_.try_lock_for(timeout);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed try_lock_for");
        switch_point("timed_mutex.try_lock_for");
        return ok;
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (current_runtime && current_runtime->manages_locks()) {
            const auto now = Clock::now();
            const auto remaining = deadline <= now
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            current_runtime->event("timed_mutex.try_lock_until object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, remaining);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed try_lock_until");
            current_runtime->switch_point("timed_mutex.try_lock_until");
            return ok;
        }

        trace_event("timed_mutex.try_lock_until object=" + identity_string());
        const bool ok = mutex_.try_lock_until(deadline);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "timed try_lock_until");
        switch_point("timed_mutex.try_lock_until");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::timed_mutex mutex_;
};

class recursive_timed_mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "recursive_timed");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_timed_mutex.lock object=" + identity_string());
            current_runtime->switch_point("recursive_timed_mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "recursive_timed");
            current_runtime->switch_point("recursive_timed_mutex.lock.after");
            return;
        }

        trace_event("recursive_timed_mutex.lock object=" + identity_string());
        switch_point("recursive_timed_mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "recursive_timed");
        switch_point("recursive_timed_mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "recursive_timed");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_timed_mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("recursive_timed_mutex.unlock");
            return;
        }

        trace_event("recursive_timed_mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("recursive_timed_mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_timed_mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "recursive_timed");
            current_runtime->switch_point("recursive_timed_mutex.try_lock");
            return ok;
        }

        trace_event("recursive_timed_mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "recursive_timed");
        switch_point("recursive_timed_mutex.try_lock");
        return ok;
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("recursive_timed_mutex.try_lock_for object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, nanos);
            publish_synchronization_event(
                SynchronizationEventKind::mutex_try_lock,
                this,
                ok,
                "recursive_timed try_lock_for"
            );
            current_runtime->switch_point("recursive_timed_mutex.try_lock_for");
            return ok;
        }

        trace_event("recursive_timed_mutex.try_lock_for object=" + identity_string());
        const bool ok = mutex_.try_lock_for(timeout);
        publish_synchronization_event(
            SynchronizationEventKind::mutex_try_lock,
            this,
            ok,
            "recursive_timed try_lock_for"
        );
        switch_point("recursive_timed_mutex.try_lock_for");
        return ok;
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (current_runtime && current_runtime->manages_locks()) {
            const auto now = Clock::now();
            const auto remaining = deadline <= now
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            current_runtime->event("recursive_timed_mutex.try_lock_until object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, remaining);
            publish_synchronization_event(
                SynchronizationEventKind::mutex_try_lock,
                this,
                ok,
                "recursive_timed try_lock_until"
            );
            current_runtime->switch_point("recursive_timed_mutex.try_lock_until");
            return ok;
        }

        trace_event("recursive_timed_mutex.try_lock_until object=" + identity_string());
        const bool ok = mutex_.try_lock_until(deadline);
        publish_synchronization_event(
            SynchronizationEventKind::mutex_try_lock,
            this,
            ok,
            "recursive_timed try_lock_until"
        );
        switch_point("recursive_timed_mutex.try_lock_until");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::recursive_timed_mutex mutex_;
};

class shared_mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "shared_mutex exclusive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.lock object=" + identity_string());
            current_runtime->switch_point("shared_mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_mutex exclusive");
            current_runtime->switch_point("shared_mutex.lock.after");
            return;
        }

        trace_event("shared_mutex.lock object=" + identity_string());
        switch_point("shared_mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_mutex exclusive");
        switch_point("shared_mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "shared_mutex exclusive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("shared_mutex.unlock");
            return;
        }

        trace_event("shared_mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("shared_mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_mutex exclusive");
            current_runtime->switch_point("shared_mutex.try_lock");
            return ok;
        }

        trace_event("shared_mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_mutex exclusive");
        switch_point("shared_mutex.try_lock");
        return ok;
    }

    void lock_shared() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "shared");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.lock_shared object=" + identity_string());
            current_runtime->switch_point("shared_mutex.lock_shared.before");
            current_runtime->lock_shared(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared");
            current_runtime->switch_point("shared_mutex.lock_shared.after");
            return;
        }

        trace_event("shared_mutex.lock_shared object=" + identity_string());
        switch_point("shared_mutex.lock_shared.before");
        mutex_.lock_shared();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared");
        switch_point("shared_mutex.lock_shared.after");
    }

    void unlock_shared() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "shared");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.unlock_shared object=" + identity_string());
            current_runtime->unlock_shared(this);
            current_runtime->switch_point("shared_mutex.unlock_shared");
            return;
        }

        trace_event("shared_mutex.unlock_shared object=" + identity_string());
        mutex_.unlock_shared();
        switch_point("shared_mutex.unlock_shared");
    }

    bool try_lock_shared() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_mutex.try_lock_shared object=" + identity_string());
            const bool ok = current_runtime->try_lock_shared(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared");
            current_runtime->switch_point("shared_mutex.try_lock_shared");
            return ok;
        }

        trace_event("shared_mutex.try_lock_shared object=" + identity_string());
        const bool ok = mutex_.try_lock_shared();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared");
        switch_point("shared_mutex.try_lock_shared");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::shared_mutex mutex_;
};

class shared_timed_mutex {
public:
    void lock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "shared_timed exclusive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.lock object=" + identity_string());
            current_runtime->switch_point("shared_timed_mutex.lock.before");
            current_runtime->lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_timed exclusive");
            current_runtime->switch_point("shared_timed_mutex.lock.after");
            return;
        }

        trace_event("shared_timed_mutex.lock object=" + identity_string());
        switch_point("shared_timed_mutex.lock.before");
        mutex_.lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_timed exclusive");
        switch_point("shared_timed_mutex.lock.after");
    }

    void unlock() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "shared_timed exclusive");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.unlock object=" + identity_string());
            current_runtime->unlock(this);
            current_runtime->switch_point("shared_timed_mutex.unlock");
            return;
        }

        trace_event("shared_timed_mutex.unlock object=" + identity_string());
        mutex_.unlock();
        switch_point("shared_timed_mutex.unlock");
    }

    bool try_lock() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.try_lock object=" + identity_string());
            const bool ok = current_runtime->try_lock(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive");
            current_runtime->switch_point("shared_timed_mutex.try_lock");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock object=" + identity_string());
        const bool ok = mutex_.try_lock();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive");
        switch_point("shared_timed_mutex.try_lock");
        return ok;
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.try_lock_for object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, nanos);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive try_lock_for");
            current_runtime->switch_point("shared_timed_mutex.try_lock_for");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock_for object=" + identity_string());
        const bool ok = mutex_.try_lock_for(timeout);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive try_lock_for");
        switch_point("shared_timed_mutex.try_lock_for");
        return ok;
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (current_runtime && current_runtime->manages_locks()) {
            const auto now = Clock::now();
            const auto remaining = deadline <= now
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            current_runtime->event("shared_timed_mutex.try_lock_until object=" + identity_string());
            const bool ok = current_runtime->try_lock_for(this, remaining);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive try_lock_until");
            current_runtime->switch_point("shared_timed_mutex.try_lock_until");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock_until object=" + identity_string());
        const bool ok = mutex_.try_lock_until(deadline);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed exclusive try_lock_until");
        switch_point("shared_timed_mutex.try_lock_until");
        return ok;
    }

    void lock_shared() {
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_attempt, this, nullptr, "shared_timed shared");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.lock_shared object=" + identity_string());
            current_runtime->switch_point("shared_timed_mutex.lock_shared.before");
            current_runtime->lock_shared(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_timed shared");
            current_runtime->switch_point("shared_timed_mutex.lock_shared.after");
            return;
        }

        trace_event("shared_timed_mutex.lock_shared object=" + identity_string());
        switch_point("shared_timed_mutex.lock_shared.before");
        mutex_.lock_shared();
        publish_synchronization_event(SynchronizationEventKind::mutex_lock_acquired, this, nullptr, "shared_timed shared");
        switch_point("shared_timed_mutex.lock_shared.after");
    }

    void unlock_shared() {
        publish_synchronization_event(SynchronizationEventKind::mutex_unlock, this, nullptr, "shared_timed shared");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.unlock_shared object=" + identity_string());
            current_runtime->unlock_shared(this);
            current_runtime->switch_point("shared_timed_mutex.unlock_shared");
            return;
        }

        trace_event("shared_timed_mutex.unlock_shared object=" + identity_string());
        mutex_.unlock_shared();
        switch_point("shared_timed_mutex.unlock_shared");
    }

    bool try_lock_shared() {
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.try_lock_shared object=" + identity_string());
            const bool ok = current_runtime->try_lock_shared(this);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared");
            current_runtime->switch_point("shared_timed_mutex.try_lock_shared");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock_shared object=" + identity_string());
        const bool ok = mutex_.try_lock_shared();
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared");
        switch_point("shared_timed_mutex.try_lock_shared");
        return ok;
    }

    template <typename Rep, typename Period>
    bool try_lock_shared_for(const std::chrono::duration<Rep, Period>& timeout) {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("shared_timed_mutex.try_lock_shared_for object=" + identity_string());
            const bool ok = current_runtime->try_lock_shared_for(this, nanos);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared try_lock_for");
            current_runtime->switch_point("shared_timed_mutex.try_lock_shared_for");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock_shared_for object=" + identity_string());
        const bool ok = mutex_.try_lock_shared_for(timeout);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared try_lock_for");
        switch_point("shared_timed_mutex.try_lock_shared_for");
        return ok;
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (current_runtime && current_runtime->manages_locks()) {
            const auto now = Clock::now();
            const auto remaining = deadline <= now
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            current_runtime->event("shared_timed_mutex.try_lock_shared_until object=" + identity_string());
            const bool ok = current_runtime->try_lock_shared_for(this, remaining);
            publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared try_lock_until");
            current_runtime->switch_point("shared_timed_mutex.try_lock_shared_until");
            return ok;
        }

        trace_event("shared_timed_mutex.try_lock_shared_until object=" + identity_string());
        const bool ok = mutex_.try_lock_shared_until(deadline);
        publish_synchronization_event(SynchronizationEventKind::mutex_try_lock, this, ok, "shared_timed shared try_lock_until");
        switch_point("shared_timed_mutex.try_lock_shared_until");
        return ok;
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::shared_timed_mutex mutex_;
};

template <typename Mutex = lincheck::mutex>
class lock_guard {
public:
    using mutex_type = Mutex;

    explicit lock_guard(mutex_type& target) : mutex_(&target) {
        mutex_->lock();
    }

    lock_guard(mutex_type& target, std::adopt_lock_t) : mutex_(&target) {}

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    ~lock_guard() {
        if (mutex_ != nullptr) {
            mutex_->unlock();
        }
    }

private:
    mutex_type* mutex_ = nullptr;
};

template <typename Mutex>
lock_guard(Mutex&) -> lock_guard<Mutex>;

template <typename Mutex>
lock_guard(Mutex&, std::adopt_lock_t) -> lock_guard<Mutex>;

template <typename... Mutexes>
class scoped_lock {
public:
    scoped_lock() requires(sizeof...(Mutexes) == 0) = default;

    explicit scoped_lock(Mutexes&... mutexes) requires(sizeof...(Mutexes) > 0)
        : mutexes_(&mutexes...) {
        lock_all();
        owns_ = true;
    }

    scoped_lock(std::adopt_lock_t, Mutexes&... mutexes) requires(sizeof...(Mutexes) > 0)
        : mutexes_(&mutexes...), owns_(true) {}

    scoped_lock(const scoped_lock&) = delete;
    scoped_lock& operator=(const scoped_lock&) = delete;

    ~scoped_lock() {
        if (owns_) {
            unlock_all();
        }
    }

private:
    void lock_all() {
        if constexpr (sizeof...(Mutexes) == 1) {
            std::get<0>(mutexes_)->lock();
        } else if constexpr (sizeof...(Mutexes) > 1) {
            std::apply([](auto*... mutexes) {
                std::lock(*mutexes...);
            }, mutexes_);
        }
    }

    void unlock_all() {
        if constexpr (sizeof...(Mutexes) > 0) {
            unlock_reverse(std::make_index_sequence<sizeof...(Mutexes)>{});
        }
    }

    template <std::size_t... I>
    void unlock_reverse(std::index_sequence<I...>) {
        (..., std::get<sizeof...(Mutexes) - 1 - I>(mutexes_)->unlock());
    }

    std::tuple<Mutexes*...> mutexes_{};
    bool owns_ = false;
};

scoped_lock() -> scoped_lock<>;

template <typename... Mutexes>
scoped_lock(Mutexes&...) -> scoped_lock<Mutexes...>;

template <typename... Mutexes>
scoped_lock(std::adopt_lock_t, Mutexes&...) -> scoped_lock<Mutexes...>;

template <typename Mutex = lincheck::mutex>
class unique_lock {
public:
    using mutex_type = Mutex;

    unique_lock() = default;

    explicit unique_lock(mutex_type& target) : mutex_(&target) {
        lock();
    }

    unique_lock(mutex_type& target, std::defer_lock_t) : mutex_(&target) {}

    unique_lock(mutex_type& target, std::try_to_lock_t) : mutex_(&target) {
        owns_ = mutex_->try_lock();
    }

    unique_lock(mutex_type& target, std::adopt_lock_t) : mutex_(&target), owns_(true) {}

    unique_lock(unique_lock&& other) noexcept
        : mutex_(std::exchange(other.mutex_, nullptr)), owns_(std::exchange(other.owns_, false)) {}

    unique_lock& operator=(unique_lock&& other) noexcept {
        if (this == &other) return *this;
        if (owns_) unlock();
        mutex_ = std::exchange(other.mutex_, nullptr);
        owns_ = std::exchange(other.owns_, false);
        return *this;
    }

    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;

    ~unique_lock() {
        if (owns_) unlock();
    }

    void lock() {
        if (mutex_ == nullptr) throw std::logic_error("unique_lock has no mutex");
        if (owns_) throw std::logic_error("unique_lock already owns mutex");
        mutex_->lock();
        owns_ = true;
    }

    bool try_lock() {
        if (mutex_ == nullptr) throw std::logic_error("unique_lock has no mutex");
        if (owns_) throw std::logic_error("unique_lock already owns mutex");
        owns_ = mutex_->try_lock();
        return owns_;
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        if (mutex_ == nullptr) throw std::logic_error("unique_lock has no mutex");
        if (owns_) throw std::logic_error("unique_lock already owns mutex");
        if constexpr (requires(mutex_type& target) { target.try_lock_for(timeout); }) {
            owns_ = mutex_->try_lock_for(timeout);
            return owns_;
        } else {
            throw std::logic_error("unique_lock timed locking requires a timed mutex");
        }
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (mutex_ == nullptr) throw std::logic_error("unique_lock has no mutex");
        if (owns_) throw std::logic_error("unique_lock already owns mutex");
        if constexpr (requires(mutex_type& target) { target.try_lock_until(deadline); }) {
            owns_ = mutex_->try_lock_until(deadline);
            return owns_;
        } else {
            throw std::logic_error("unique_lock timed locking requires a timed mutex");
        }
    }

    void unlock() {
        if (mutex_ == nullptr || !owns_) throw std::logic_error("unique_lock does not own mutex");
        mutex_->unlock();
        owns_ = false;
    }

    bool owns_lock() const {
        return owns_;
    }

    explicit operator bool() const {
        return owns_lock();
    }

    mutex_type* release() {
        owns_ = false;
        return std::exchange(mutex_, nullptr);
    }

    mutex_type* mutex() const {
        return mutex_;
    }

    mutex_type* mutex_ptr() const {
        return mutex();
    }

    void swap(unique_lock& other) noexcept {
        std::swap(mutex_, other.mutex_);
        std::swap(owns_, other.owns_);
    }

    friend void swap(unique_lock& first, unique_lock& second) noexcept {
        first.swap(second);
    }

private:
    friend class condition_variable;

    const void* lock_identity() const {
        return mutex_;
    }

    mutex_type* mutex_ = nullptr;
    bool owns_ = false;
};

template <typename Mutex>
unique_lock(Mutex&) -> unique_lock<Mutex>;

template <typename Mutex>
unique_lock(Mutex&, std::defer_lock_t) -> unique_lock<Mutex>;

template <typename Mutex>
unique_lock(Mutex&, std::try_to_lock_t) -> unique_lock<Mutex>;

template <typename Mutex>
unique_lock(Mutex&, std::adopt_lock_t) -> unique_lock<Mutex>;

template <typename Mutex = lincheck::shared_mutex>
class shared_lock {
public:
    using mutex_type = Mutex;

    shared_lock() = default;

    explicit shared_lock(mutex_type& target) : mutex_(&target) {
        lock();
    }

    shared_lock(mutex_type& target, std::defer_lock_t) : mutex_(&target) {}

    shared_lock(mutex_type& target, std::try_to_lock_t) : mutex_(&target) {
        owns_ = mutex_->try_lock_shared();
    }

    shared_lock(mutex_type& target, std::adopt_lock_t) : mutex_(&target), owns_(true) {}

    template <typename Rep, typename Period>
    shared_lock(mutex_type& target, const std::chrono::duration<Rep, Period>& timeout)
        : mutex_(&target) {
        owns_ = try_lock_for(timeout);
    }

    template <typename Clock, typename Duration>
    shared_lock(mutex_type& target, const std::chrono::time_point<Clock, Duration>& deadline)
        : mutex_(&target) {
        owns_ = try_lock_until(deadline);
    }

    shared_lock(shared_lock&& other) noexcept
        : mutex_(std::exchange(other.mutex_, nullptr)), owns_(std::exchange(other.owns_, false)) {}

    shared_lock& operator=(shared_lock&& other) noexcept {
        if (this == &other) return *this;
        if (owns_) unlock();
        mutex_ = std::exchange(other.mutex_, nullptr);
        owns_ = std::exchange(other.owns_, false);
        return *this;
    }

    shared_lock(const shared_lock&) = delete;
    shared_lock& operator=(const shared_lock&) = delete;

    ~shared_lock() {
        if (owns_) unlock();
    }

    void lock() {
        if (mutex_ == nullptr) throw std::logic_error("shared_lock has no mutex");
        if (owns_) throw std::logic_error("shared_lock already owns mutex");
        mutex_->lock_shared();
        owns_ = true;
    }

    bool try_lock() {
        if (mutex_ == nullptr) throw std::logic_error("shared_lock has no mutex");
        if (owns_) throw std::logic_error("shared_lock already owns mutex");
        owns_ = mutex_->try_lock_shared();
        return owns_;
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        if (mutex_ == nullptr) throw std::logic_error("shared_lock has no mutex");
        if (owns_) throw std::logic_error("shared_lock already owns mutex");
        if constexpr (requires(mutex_type& target) { target.try_lock_shared_for(timeout); }) {
            owns_ = mutex_->try_lock_shared_for(timeout);
            return owns_;
        } else {
            throw std::logic_error("shared_lock timed locking requires a shared timed mutex");
        }
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        if (mutex_ == nullptr) throw std::logic_error("shared_lock has no mutex");
        if (owns_) throw std::logic_error("shared_lock already owns mutex");
        if constexpr (requires(mutex_type& target) { target.try_lock_shared_until(deadline); }) {
            owns_ = mutex_->try_lock_shared_until(deadline);
            return owns_;
        } else {
            throw std::logic_error("shared_lock timed locking requires a shared timed mutex");
        }
    }

    void unlock() {
        if (mutex_ == nullptr || !owns_) throw std::logic_error("shared_lock does not own mutex");
        mutex_->unlock_shared();
        owns_ = false;
    }

    bool owns_lock() const {
        return owns_;
    }

    explicit operator bool() const {
        return owns_lock();
    }

    mutex_type* release() {
        owns_ = false;
        return std::exchange(mutex_, nullptr);
    }

    mutex_type* mutex() const {
        return mutex_;
    }

    mutex_type* mutex_ptr() const {
        return mutex();
    }

    void swap(shared_lock& other) noexcept {
        std::swap(mutex_, other.mutex_);
        std::swap(owns_, other.owns_);
    }

    friend void swap(shared_lock& first, shared_lock& second) noexcept {
        first.swap(second);
    }

private:
    mutex_type* mutex_ = nullptr;
    bool owns_ = false;
};

template <typename Mutex>
shared_lock(Mutex&) -> shared_lock<Mutex>;

template <typename Mutex>
shared_lock(Mutex&, std::defer_lock_t) -> shared_lock<Mutex>;

template <typename Mutex>
shared_lock(Mutex&, std::try_to_lock_t) -> shared_lock<Mutex>;

template <typename Mutex>
shared_lock(Mutex&, std::adopt_lock_t) -> shared_lock<Mutex>;

template <typename Mutex, typename Rep, typename Period>
shared_lock(Mutex&, const std::chrono::duration<Rep, Period>&) -> shared_lock<Mutex>;

template <typename Mutex, typename Clock, typename Duration>
shared_lock(Mutex&, const std::chrono::time_point<Clock, Duration>&) -> shared_lock<Mutex>;

class condition_variable {
public:
    condition_variable() = default;
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    template <typename Mutex>
    void wait(unique_lock<Mutex>& lock) {
        if (!lock.owns_lock()) throw std::logic_error("condition_variable wait requires an owned lock");
        publish_synchronization_event(SynchronizationEventKind::condition_wait, this, lock.lock_identity());
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event(wait_event("condition_variable.wait", lock));
            try {
                current_runtime->wait_condition(this, lock.lock_identity());
            } catch (...) {
                if (!current_runtime->owns_lock(lock.lock_identity())) {
                    lock.owns_ = false;
                }
                throw;
            }
            publish_synchronization_event(SynchronizationEventKind::condition_wake, this, lock.lock_identity());
            current_runtime->switch_point("condition_variable.wait");
            return;
        }

        trace_event(wait_event("condition_variable.wait", lock));
        condition_.wait(lock);
        publish_synchronization_event(SynchronizationEventKind::condition_wake, this, lock.lock_identity());
        switch_point("condition_variable.wait");
    }

    template <typename Mutex, typename Predicate>
    void wait(unique_lock<Mutex>& lock, Predicate predicate) {
        while (!predicate()) {
            wait(lock);
        }
    }

    template <typename Mutex, typename Rep, typename Period>
    std::cv_status wait_for(unique_lock<Mutex>& lock, const std::chrono::duration<Rep, Period>& duration) {
        if (!lock.owns_lock()) throw std::logic_error("condition_variable wait_for requires an owned lock");
        publish_synchronization_event(SynchronizationEventKind::condition_wait, this, lock.lock_identity(), "wait_for");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event(wait_event("condition_variable.wait_for", lock));
            std::cv_status status = std::cv_status::no_timeout;
            try {
                status = current_runtime->wait_condition_for(
                    this,
                    lock.lock_identity(),
                    std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                );
            } catch (...) {
                if (!current_runtime->owns_lock(lock.lock_identity())) {
                    lock.owns_ = false;
                }
                throw;
            }
            publish_synchronization_event(
                SynchronizationEventKind::condition_wake,
                this,
                lock.lock_identity(),
                status == std::cv_status::timeout ? "timeout" : "no_timeout"
            );
            current_runtime->switch_point("condition_variable.wait_for");
            return status;
        }

        trace_event(wait_event("condition_variable.wait_for", lock));
        const auto status = condition_.wait_for(lock, duration);
        publish_synchronization_event(
            SynchronizationEventKind::condition_wake,
            this,
            lock.lock_identity(),
            status == std::cv_status::timeout ? "timeout" : "no_timeout"
        );
        switch_point("condition_variable.wait_for");
        return status;
    }

    template <typename Mutex, typename Rep, typename Period, typename Predicate>
    bool wait_for(unique_lock<Mutex>& lock, const std::chrono::duration<Rep, Period>& duration, Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (!predicate()) {
            if (wait_until(lock, deadline) == std::cv_status::timeout) {
                return predicate();
            }
        }
        return true;
    }

    template <typename Mutex, typename Clock, typename Duration>
    std::cv_status wait_until(unique_lock<Mutex>& lock, const std::chrono::time_point<Clock, Duration>& deadline) {
        if (!lock.owns_lock()) throw std::logic_error("condition_variable wait_until requires an owned lock");
        publish_synchronization_event(SynchronizationEventKind::condition_wait, this, lock.lock_identity(), "wait_until");
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event(wait_event("condition_variable.wait_until", lock));
            const auto now = Clock::now();
            const auto remaining = deadline <= now
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            std::cv_status status = std::cv_status::no_timeout;
            try {
                status = current_runtime->wait_condition_for(this, lock.lock_identity(), remaining);
            } catch (...) {
                if (!current_runtime->owns_lock(lock.lock_identity())) {
                    lock.owns_ = false;
                }
                throw;
            }
            publish_synchronization_event(
                SynchronizationEventKind::condition_wake,
                this,
                lock.lock_identity(),
                status == std::cv_status::timeout ? "timeout" : "no_timeout"
            );
            current_runtime->switch_point("condition_variable.wait_until");
            return status;
        }

        trace_event(wait_event("condition_variable.wait_until", lock));
        const auto status = condition_.wait_until(lock, deadline);
        publish_synchronization_event(
            SynchronizationEventKind::condition_wake,
            this,
            lock.lock_identity(),
            status == std::cv_status::timeout ? "timeout" : "no_timeout"
        );
        switch_point("condition_variable.wait_until");
        return status;
    }

    template <typename Mutex, typename Clock, typename Duration, typename Predicate>
    bool wait_until(unique_lock<Mutex>& lock, const std::chrono::time_point<Clock, Duration>& deadline, Predicate predicate) {
        while (!predicate()) {
            if (wait_until(lock, deadline) == std::cv_status::timeout) {
                return predicate();
            }
        }
        return true;
    }

    void notify_one() {
        publish_synchronization_event(SynchronizationEventKind::condition_notify_one, this);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("condition_variable.notify_one object=" + identity_string());
            current_runtime->notify_one_condition(this);
            current_runtime->switch_point("condition_variable.notify_one");
            return;
        }

        trace_event("condition_variable.notify_one object=" + identity_string());
        condition_.notify_one();
        switch_point("condition_variable.notify_one");
    }

    void notify_all() {
        publish_synchronization_event(SynchronizationEventKind::condition_notify_all, this);
        if (current_runtime && current_runtime->manages_locks()) {
            current_runtime->event("condition_variable.notify_all object=" + identity_string());
            current_runtime->notify_all_condition(this);
            current_runtime->switch_point("condition_variable.notify_all");
            return;
        }

        trace_event("condition_variable.notify_all object=" + identity_string());
        condition_.notify_all();
        switch_point("condition_variable.notify_all");
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    template <typename Mutex>
    std::string wait_event(const char* name, const unique_lock<Mutex>& lock) const {
        return std::string(name) +
            " object=" + identity_string() +
            " lock=" + stable_object_id(lock.lock_identity());
    }

    std::condition_variable_any condition_;
};

class parker {
public:
    parker() = default;
    parker(const parker&) = delete;
    parker& operator=(const parker&) = delete;

    void park() {
        publish_synchronization_event(SynchronizationEventKind::parker_park, this);
        if (current_runtime && current_runtime->manages_parking()) {
            current_runtime->event("parker.park object=" + identity_string());
            current_runtime->park(this);
            current_runtime->switch_point("parker.park");
            return;
        }

        trace_event("parker.park object=" + identity_string());
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return permit_; });
            permit_ = false;
        }
        switch_point("parker.park");
    }

    void unpark() {
        publish_synchronization_event(SynchronizationEventKind::parker_unpark, this);
        if (current_runtime && current_runtime->manages_parking()) {
            current_runtime->event("parker.unpark object=" + identity_string());
            current_runtime->unpark(this);
            current_runtime->switch_point("parker.unpark");
            return;
        }

        trace_event("parker.unpark object=" + identity_string());
        {
            std::lock_guard lock(mutex_);
            permit_ = true;
        }
        condition_.notify_one();
        switch_point("parker.unpark");
    }

private:
    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    bool permit_ = false;
};

inline void park(parker& target) {
    target.park();
}

inline void unpark(parker& target) {
    target.unpark();
}

template <std::ptrdiff_t LeastMaxValue = std::numeric_limits<std::ptrdiff_t>::max()>
class counting_semaphore {
    static_assert(LeastMaxValue >= 0, "lincheck::counting_semaphore requires a non-negative max value");

public:
    explicit counting_semaphore(std::ptrdiff_t desired) : permits_(desired) {
        validate_permit_count(desired, "counting_semaphore initial permit count");
    }

    counting_semaphore(const counting_semaphore&) = delete;
    counting_semaphore& operator=(const counting_semaphore&) = delete;

    static constexpr std::ptrdiff_t max() noexcept {
        return LeastMaxValue;
    }

    void release(std::ptrdiff_t update = 1) {
        validate_release_update(update);
        const auto observed = observed_permits();
        publish_synchronization_event(
            SynchronizationEventKind::semaphore_release,
            this,
            nullptr,
            "update=" + std::to_string(update)
        );

        if (current_runtime && current_runtime->manages_semaphores()) {
            add_permits(update);
            current_runtime->event(release_event("semaphore.release", update));
            current_runtime->release_semaphore(this, update, observed);
            current_runtime->switch_point("semaphore.release");
            return;
        }

        trace_event(release_event("semaphore.release", update));
        add_permits(update);
        switch_point("semaphore.release");
    }

    void acquire() {
        const auto observed = observed_permits();
        if (current_runtime && current_runtime->manages_semaphores()) {
            current_runtime->event(operation_event("semaphore.acquire"));
            current_runtime->acquire_semaphore(this, observed);
            consume_permit_after_managed_acquire();
            publish_synchronization_event(SynchronizationEventKind::semaphore_acquire, this, true);
            current_runtime->switch_point("semaphore.acquire");
            return;
        }

        trace_event(operation_event("semaphore.acquire"));
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return permits_ > 0; });
            --permits_;
        }
        publish_synchronization_event(SynchronizationEventKind::semaphore_acquire, this, true);
        switch_point("semaphore.acquire");
    }

    bool try_acquire() {
        bool acquired = false;
        const auto observed = observed_permits();
        if (current_runtime && current_runtime->manages_semaphores()) {
            current_runtime->event(operation_event("semaphore.try_acquire"));
            acquired = current_runtime->try_acquire_semaphore(this, observed);
            if (acquired) {
                consume_permit_after_managed_acquire();
            }
            publish_synchronization_event(SynchronizationEventKind::semaphore_try_acquire, this, acquired);
            current_runtime->switch_point("semaphore.try_acquire");
            return acquired;
        }

        trace_event(operation_event("semaphore.try_acquire"));
        {
            std::lock_guard lock(mutex_);
            if (permits_ > 0) {
                --permits_;
                acquired = true;
            }
        }
        publish_synchronization_event(SynchronizationEventKind::semaphore_try_acquire, this, acquired);
        switch_point("semaphore.try_acquire");
        return acquired;
    }

    template <typename Rep, typename Period>
    bool try_acquire_for(const std::chrono::duration<Rep, Period>& duration) {
        const auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
        bool acquired = false;
        const auto observed = observed_permits();
        if (current_runtime && current_runtime->manages_semaphores()) {
            current_runtime->event(operation_event("semaphore.try_acquire_for"));
            acquired = current_runtime->try_acquire_semaphore_for(this, observed, timeout);
            if (acquired) {
                consume_permit_after_managed_acquire();
            }
            publish_synchronization_event(
                SynchronizationEventKind::semaphore_try_acquire,
                this,
                acquired,
                "try_acquire_for"
            );
            current_runtime->switch_point("semaphore.try_acquire_for");
            return acquired;
        }

        trace_event(operation_event("semaphore.try_acquire_for"));
        {
            std::unique_lock lock(mutex_);
            acquired = condition_.wait_for(lock, duration, [&] { return permits_ > 0; });
            if (acquired) {
                --permits_;
            }
        }
        publish_synchronization_event(
            SynchronizationEventKind::semaphore_try_acquire,
            this,
            acquired,
            "try_acquire_for"
        );
        switch_point("semaphore.try_acquire_for");
        return acquired;
    }

    template <typename Clock, typename Duration>
    bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& deadline) {
        const auto now = Clock::now();
        const auto remaining = deadline <= now
            ? std::chrono::nanoseconds::zero()
            : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        return try_acquire_for(remaining);
    }

private:
    static void validate_permit_count(std::ptrdiff_t value, const char* name) {
        if (value < 0 || value > max()) {
            throw std::invalid_argument(std::string("lincheck ") + name + " is outside [0, max()]");
        }
    }

    static void validate_release_update(std::ptrdiff_t update) {
        if (update < 0) {
            throw std::invalid_argument("lincheck counting_semaphore release update must be non-negative");
        }
    }

    std::ptrdiff_t observed_permits() const {
        std::lock_guard lock(mutex_);
        return permits_;
    }

    void add_permits(std::ptrdiff_t update) {
        if (update == 0) return;
        {
            std::lock_guard lock(mutex_);
            if (permits_ > max() - update) {
                throw std::invalid_argument("lincheck counting_semaphore release would exceed max()");
            }
            permits_ += update;
        }
        condition_.notify_all();
    }

    void consume_permit_after_managed_acquire() {
        std::lock_guard lock(mutex_);
        if (permits_ <= 0) {
            throw std::runtime_error("lincheck managed semaphore permit count is inconsistent");
        }
        --permits_;
    }

    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::string operation_event(const char* name) const {
        return std::string(name) + " object=" + identity_string();
    }

    std::string release_event(const char* name, std::ptrdiff_t update) const {
        return operation_event(name) + " update=" + std::to_string(update);
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::ptrdiff_t permits_ = 0;
};

using binary_semaphore = counting_semaphore<1>;

class latch {
public:
    explicit latch(std::ptrdiff_t expected) : count_(expected) {
        validate_count(expected, "latch initial count");
    }

    latch(const latch&) = delete;
    latch& operator=(const latch&) = delete;

    void count_down(std::ptrdiff_t update = 1) {
        validate_count_down(update);
        const auto observed = observed_count();
        publish_synchronization_event(
            SynchronizationEventKind::latch_count_down,
            this,
            nullptr,
            "update=" + std::to_string(update)
        );

        if (current_runtime && current_runtime->manages_latches()) {
            subtract_count(update);
            current_runtime->event(count_down_event("latch.count_down", update));
            current_runtime->count_down_latch(this, update, observed);
            current_runtime->switch_point("latch.count_down");
            return;
        }

        trace_event(count_down_event("latch.count_down", update));
        subtract_count(update);
        switch_point("latch.count_down");
    }

    bool try_wait() const {
        const auto observed = observed_count();
        bool ready = observed == 0;
        if (current_runtime && current_runtime->manages_latches()) {
            current_runtime->event(operation_event("latch.try_wait"));
            ready = current_runtime->try_wait_latch(this, observed);
            publish_synchronization_event(SynchronizationEventKind::latch_try_wait, this, ready);
            current_runtime->switch_point("latch.try_wait");
            return ready;
        }

        trace_event(operation_event("latch.try_wait"));
        publish_synchronization_event(SynchronizationEventKind::latch_try_wait, this, ready);
        switch_point("latch.try_wait");
        return ready;
    }

    void wait() const {
        const auto observed = observed_count();
        publish_synchronization_event(SynchronizationEventKind::latch_wait, this);
        if (current_runtime && current_runtime->manages_latches()) {
            current_runtime->event(operation_event("latch.wait"));
            current_runtime->wait_latch(this, observed);
            publish_synchronization_event(SynchronizationEventKind::latch_wake, this);
            current_runtime->switch_point("latch.wait");
            return;
        }

        trace_event(operation_event("latch.wait"));
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return count_ == 0; });
        }
        publish_synchronization_event(SynchronizationEventKind::latch_wake, this);
        switch_point("latch.wait");
    }

    void arrive_and_wait(std::ptrdiff_t update = 1) {
        count_down(update);
        wait();
    }

private:
    static void validate_count(std::ptrdiff_t value, const char* name) {
        if (value < 0) {
            throw std::invalid_argument(std::string("lincheck ") + name + " must be non-negative");
        }
    }

    static void validate_count_down(std::ptrdiff_t update) {
        if (update < 0) {
            throw std::invalid_argument("lincheck latch count_down update must be non-negative");
        }
    }

    std::ptrdiff_t observed_count() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    void subtract_count(std::ptrdiff_t update) {
        bool became_ready = false;
        {
            std::lock_guard lock(mutex_);
            if (update > count_) {
                throw std::invalid_argument("lincheck latch count_down update exceeds the current count");
            }
            count_ -= update;
            became_ready = count_ == 0;
        }
        if (became_ready) {
            condition_.notify_all();
        }
    }

    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::string operation_event(const char* name) const {
        return std::string(name) + " object=" + identity_string();
    }

    std::string count_down_event(const char* name, std::ptrdiff_t update) const {
        return operation_event(name) + " update=" + std::to_string(update);
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::ptrdiff_t count_ = 0;
};

namespace detail {

struct BarrierNoCompletion {
    void operator()() const noexcept {}
};

template <typename CompletionFunction>
inline constexpr bool is_noop_barrier_completion_v =
    std::is_same_v<std::decay_t<CompletionFunction>, BarrierNoCompletion>;

} // namespace detail

template <typename CompletionFunction = detail::BarrierNoCompletion>
class barrier {
public:
    class arrival_token {
    public:
        arrival_token() = delete;
        arrival_token(const arrival_token&) = delete;
        arrival_token& operator=(const arrival_token&) = delete;

        arrival_token(arrival_token&& other) noexcept
            : barrier_(other.barrier_), phase_(other.phase_), valid_(other.valid_) {
            other.valid_ = false;
            other.barrier_ = nullptr;
        }

        arrival_token& operator=(arrival_token&& other) noexcept {
            if (this == &other) return *this;
            barrier_ = other.barrier_;
            phase_ = other.phase_;
            valid_ = other.valid_;
            other.valid_ = false;
            other.barrier_ = nullptr;
            return *this;
        }

    private:
        friend class barrier;

        arrival_token(const barrier* owner, std::size_t phase)
            : barrier_(owner), phase_(phase), valid_(true) {}

        const barrier* barrier_ = nullptr;
        std::size_t phase_ = 0;
        bool valid_ = false;
    };

    explicit barrier(
        std::ptrdiff_t expected,
        CompletionFunction completion = CompletionFunction()
    ) : completion_(std::move(completion)), expected_(expected), remaining_(expected) {
        validate_expected(expected);
    }

    barrier(const barrier&) = delete;
    barrier& operator=(const barrier&) = delete;

    static constexpr std::ptrdiff_t max() noexcept {
        return std::numeric_limits<std::ptrdiff_t>::max();
    }

    arrival_token arrive(std::ptrdiff_t update = 1) {
        return arrive_impl(update, false);
    }

    void wait(arrival_token&& arrival) const {
        if (!arrival.valid_ || arrival.barrier_ != this) {
            throw std::invalid_argument("lincheck barrier wait requires a valid arrival_token from this barrier");
        }

        const auto phase = arrival.phase_;
        arrival.valid_ = false;
        arrival.barrier_ = nullptr;
        publish_synchronization_event(
            SynchronizationEventKind::barrier_wait,
            this,
            nullptr,
            "phase=" + std::to_string(phase)
        );

        if (current_runtime && current_runtime->manages_barriers()) {
            current_runtime->event(phase_event("barrier.wait", phase));
            current_runtime->wait_barrier(this, phase);
            publish_synchronization_event(
                SynchronizationEventKind::barrier_wake,
                this,
                nullptr,
                "phase=" + std::to_string(phase)
            );
            current_runtime->switch_point("barrier.wait");
            return;
        }

        trace_event(phase_event("barrier.wait", phase));
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return phase_ != phase; });
        }
        publish_synchronization_event(
            SynchronizationEventKind::barrier_wake,
            this,
            nullptr,
            "phase=" + std::to_string(phase)
        );
        switch_point("barrier.wait");
    }

    void arrive_and_wait() {
        auto arrival = arrive();
        wait(std::move(arrival));
    }

    void arrive_and_drop() {
        (void)arrive_impl(1, true);
    }

private:
    struct ArrivalObservation {
        std::size_t phase = 0;
        std::ptrdiff_t expected = 0;
        std::ptrdiff_t remaining = 0;
    };

    struct ArrivalResult {
        arrival_token token;
        ArrivalObservation observed;
        bool completed = false;
    };

    static void validate_expected(std::ptrdiff_t expected) {
        if (expected < 0 || expected > max()) {
            throw std::invalid_argument("lincheck barrier expected count is outside [0, max()]");
        }
    }

    static void validate_arrival_update(std::ptrdiff_t update) {
        if (update <= 0) {
            throw std::invalid_argument("lincheck barrier arrive update must be positive");
        }
    }

    arrival_token arrive_impl(std::ptrdiff_t update, bool drop) {
        validate_arrival_update(update);
        const bool managed = current_runtime && current_runtime->manages_barriers();
        auto result = apply_arrival(update, drop, !managed);
        const auto detail = arrival_detail(update, drop, result.observed.phase);
        publish_synchronization_event(
            drop ? SynchronizationEventKind::barrier_drop : SynchronizationEventKind::barrier_arrive,
            this,
            nullptr,
            detail
        );

        if (current_runtime && current_runtime->manages_barriers()) {
            current_runtime->event(arrival_event(drop ? "barrier.arrive_and_drop" : "barrier.arrive", update, drop));
            current_runtime->arrive_barrier(
                this,
                update,
                drop,
                result.observed.expected,
                result.observed.remaining,
                result.observed.phase
            );
            if (result.completed) {
                publish_synchronization_event(
                    SynchronizationEventKind::barrier_phase_complete,
                    this,
                    nullptr,
                    "phase=" + std::to_string(result.observed.phase)
                );
            }
            current_runtime->switch_point(drop ? "barrier.arrive_and_drop" : "barrier.arrive");
            return std::move(result.token);
        }

        trace_event(arrival_event(drop ? "barrier.arrive_and_drop" : "barrier.arrive", update, drop));
        if (result.completed) {
            publish_synchronization_event(
                SynchronizationEventKind::barrier_phase_complete,
                this,
                nullptr,
                "phase=" + std::to_string(result.observed.phase)
            );
        }
        switch_point(drop ? "barrier.arrive_and_drop" : "barrier.arrive");
        return std::move(result.token);
    }

    ArrivalResult apply_arrival(std::ptrdiff_t update, bool drop, bool invoke_completion) {
        bool notify = false;
        ArrivalResult result{arrival_token(this, 0), {}, false};
        {
            std::unique_lock lock(mutex_);
            result.observed.phase = phase_;
            result.observed.expected = expected_;
            result.observed.remaining = remaining_;
            result.token.phase_ = phase_;

            if (drop && expected_ <= 0) {
                throw std::invalid_argument("lincheck barrier arrive_and_drop requires a positive expected count");
            }
            if (update > remaining_) {
                throw std::invalid_argument("lincheck barrier arrive update exceeds the current phase count");
            }
            if (
                !invoke_completion &&
                !detail::is_noop_barrier_completion_v<CompletionFunction> &&
                remaining_ == update
            ) {
                throw std::invalid_argument("lincheck managed barrier does not support custom completion functions");
            }

            if (drop) {
                --expected_;
            }
            remaining_ -= update;
            if (remaining_ == 0) {
                if (invoke_completion) {
                    completion_();
                }
                ++phase_;
                remaining_ = expected_;
                result.completed = true;
                notify = true;
            }
        }
        if (notify) {
            condition_.notify_all();
        }
        return result;
    }

    std::string identity_string() const {
        return stable_object_id(this);
    }

    std::string arrival_event(const char* name, std::ptrdiff_t update, bool drop) const {
        std::string result = std::string(name) + " object=" + identity_string() +
            " update=" + std::to_string(update);
        if (drop) {
            result += " drop";
        }
        return result;
    }

    std::string phase_event(const char* name, std::size_t phase) const {
        return std::string(name) + " object=" + identity_string() +
            " phase=" + std::to_string(phase);
    }

    static std::string arrival_detail(std::ptrdiff_t update, bool drop, std::size_t phase) {
        std::string result = "update=" + std::to_string(update) +
            " phase=" + std::to_string(phase);
        if (drop) {
            result += " drop";
        }
        return result;
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    CompletionFunction completion_;
    std::ptrdiff_t expected_ = 0;
    std::ptrdiff_t remaining_ = 0;
    std::size_t phase_ = 0;
};

barrier(std::ptrdiff_t) -> barrier<detail::BarrierNoCompletion>;

template <typename CompletionFunction>
barrier(std::ptrdiff_t, CompletionFunction) -> barrier<CompletionFunction>;

class thread {
public:
    using id = std::thread::id;
    using native_handle_type = std::thread::native_handle_type;

    thread() = default;

    template <typename Fn, typename... Args>
        requires (!std::is_same_v<std::decay_t<Fn>, thread>)
    explicit thread(Fn&& fn, Args&&... args) : exception_(std::make_shared<std::exception_ptr>()) {
        auto exception = exception_;
        auto task = [
            fn = std::decay_t<Fn>(std::forward<Fn>(fn)),
            args = std::make_tuple(std::forward<Args>(args)...)
        ]() mutable {
            std::apply([&](auto&... unpacked) {
                std::invoke(fn, unpacked...);
            }, args);
        };
        thread_ = std::thread([
            runtime = current_runtime,
            warning_sink = current_warning_sink,
            task = std::move(task),
            exception
        ]() mutable {
            ScopedRuntime scoped(runtime);
            ScopedWarningSink warning_scoped(warning_sink);
            trace_event("thread.start");
            try {
                task();
            } catch (...) {
                *exception = std::current_exception();
                trace_event("thread.exception");
            }
            trace_event("thread.finish");
        });
    }

    thread(thread&&) noexcept = default;
    thread& operator=(thread&&) noexcept = default;

    ~thread() {
        if (thread_.joinable()) thread_.join();
    }

    void join() {
        trace_event("thread.join");
        if (thread_.joinable()) thread_.join();
        switch_point("thread.join");
        if (exception_ && *exception_) {
            std::rethrow_exception(*exception_);
        }
    }

    void detach() {
        trace_event("thread.detach");
        thread_.detach();
        switch_point("thread.detach");
    }

    bool joinable() const noexcept {
        return thread_.joinable();
    }

    id get_id() const noexcept {
        return thread_.get_id();
    }

    native_handle_type native_handle() {
        return thread_.native_handle();
    }

    void swap(thread& other) noexcept {
        thread_.swap(other.thread_);
        exception_.swap(other.exception_);
    }

    friend void swap(thread& first, thread& second) noexcept {
        first.swap(second);
    }

private:
    std::thread thread_;
    std::shared_ptr<std::exception_ptr> exception_;
};

class jthread {
public:
    using id = std::thread::id;
    using native_handle_type = std::jthread::native_handle_type;

    jthread() noexcept = default;

    template <typename Fn, typename... Args>
        requires (!std::is_same_v<std::decay_t<Fn>, jthread>)
    explicit jthread(Fn&& fn, Args&&... args) : exception_(std::make_shared<std::exception_ptr>()) {
        auto exception = exception_;
        auto task = [
            fn = std::decay_t<Fn>(std::forward<Fn>(fn)),
            args = std::make_tuple(std::forward<Args>(args)...)
        ](std::stop_token token) mutable {
            std::apply([&](auto&... unpacked) {
                if constexpr (std::is_invocable_v<decltype(fn)&, std::stop_token, decltype(unpacked)...>) {
                    std::invoke(fn, token, unpacked...);
                } else {
                    std::invoke(fn, unpacked...);
                }
            }, args);
        };
        thread_ = std::jthread([
            runtime = current_runtime,
            warning_sink = current_warning_sink,
            task = std::move(task),
            exception
        ](std::stop_token token) mutable {
            ScopedRuntime scoped(runtime);
            ScopedWarningSink warning_scoped(warning_sink);
            trace_event("jthread.start");
            try {
                task(token);
            } catch (...) {
                *exception = std::current_exception();
                trace_event("jthread.exception");
            }
            trace_event("jthread.finish");
        });
    }

    jthread(jthread&&) noexcept = default;
    jthread& operator=(jthread&&) noexcept = default;

    ~jthread() = default;

    void join() {
        trace_event("jthread.join");
        if (thread_.joinable()) thread_.join();
        switch_point("jthread.join");
        if (exception_ && *exception_) {
            std::rethrow_exception(*exception_);
        }
    }

    void detach() {
        trace_event("jthread.detach");
        thread_.detach();
        switch_point("jthread.detach");
    }

    bool joinable() const noexcept {
        return thread_.joinable();
    }

    id get_id() const noexcept {
        return thread_.get_id();
    }

    native_handle_type native_handle() {
        return thread_.native_handle();
    }

    std::stop_source get_stop_source() noexcept {
        return thread_.get_stop_source();
    }

    std::stop_token get_stop_token() const noexcept {
        return thread_.get_stop_token();
    }

    bool request_stop() noexcept {
        trace_event("jthread.request_stop");
        return thread_.request_stop();
    }

    bool stop_requested() const noexcept {
        return thread_.get_stop_token().stop_requested();
    }

    bool stop_possible() const noexcept {
        return thread_.get_stop_token().stop_possible();
    }

    void swap(jthread& other) noexcept {
        thread_.swap(other.thread_);
        exception_.swap(other.exception_);
    }

    friend void swap(jthread& first, jthread& second) noexcept {
        first.swap(second);
    }

private:
    std::jthread thread_;
    std::shared_ptr<std::exception_ptr> exception_;
};

namespace this_thread {

inline thread::id get_id() noexcept {
    return std::this_thread::get_id();
}

inline void yield() {
    ::lincheck::yield();
}

inline void yield(const char* location) {
    ::lincheck::yield(location);
}

inline void yield(const SourceLocation& source) {
    ::lincheck::yield(source);
}

namespace detail {

inline bool runtime_manages_waits() {
    return current_runtime != nullptr &&
        (current_runtime->manages_locks() ||
         current_runtime->manages_parking() ||
         current_runtime->manages_atomic_waits());
}

template <typename Duration>
std::string sleep_duration_event(std::string name, const Duration& duration) {
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return std::move(name) + " duration_ns=" + std::to_string(nanos);
}

} // namespace detail

template <typename Rep, typename Period>
void sleep_for(const std::chrono::duration<Rep, Period>& duration) {
    trace_event(detail::sleep_duration_event("this_thread.sleep_for", duration));
    switch_point("this_thread.sleep_for");
    if (!detail::runtime_manages_waits()) {
        std::this_thread::sleep_for(duration);
    }
}

template <typename Clock, typename Duration>
void sleep_until(const std::chrono::time_point<Clock, Duration>& deadline) {
    trace_event("this_thread.sleep_until");
    switch_point("this_thread.sleep_until");
    if (!detail::runtime_manages_waits()) {
        std::this_thread::sleep_until(deadline);
    }
}

} // namespace this_thread

struct OptionsConfig {
    int iterations = 100;
    int invocations_per_iteration = 1000;
    int threads = 2;
    int actors_per_thread = 5;
    int actors_before = 0;
    int actors_after = 0;
    std::uint64_t seed = 0;
    int max_schedule_length = 6;
    int max_switch_points_per_schedule = 10000;
    int max_context_switches_per_schedule = -1;
    bool minimize_failed_scenario = true;
    ClockSourceKind clock_source = ClockSourceKind::atomic_sequence;
    MemoryModel memory_model = MemoryModel::sequential_consistency;
    bool operation_context_reduction = false;
    bool event_dependency_reduction = false;
    bool check_obstruction_freedom = false;
    int obstruction_switch_bound = 1000;
    std::chrono::milliseconds invocation_timeout{0};
    TraceFilter trace_filter;
};

namespace detail {

inline void require_option_at_least(std::string_view name, int value, int minimum) {
    if (value < minimum) {
        throw std::invalid_argument(
            "lincheck option " + std::string(name) +
            " must be >= " + std::to_string(minimum)
        );
    }
}

inline void require_positive_option(std::string_view name, int value) {
    require_option_at_least(name, value, 1);
}

inline void require_non_negative_option(std::string_view name, int value) {
    require_option_at_least(name, value, 0);
}

inline void require_supported_memory_model(MemoryModel model) {
    if (model != MemoryModel::sequential_consistency) {
        throw std::invalid_argument(
            "lincheck option memory_model=" + std::string(memory_model_name(model)) +
            " is not implemented; current checking explores only sequentially consistent wrapper behavior"
        );
    }
}

} // namespace detail

class StressOptions;
class ModelCheckingOptions;

namespace detail {

inline std::string exception_message(const std::exception_ptr& ptr) {
    if (!ptr) return "unknown exception";
    try {
        std::rethrow_exception(ptr);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "non-std exception";
    }
}

inline FailureKind failure_kind_from_exception(const std::exception_ptr& ptr) {
    if (!ptr) return FailureKind::unexpected_exception;
    try {
        std::rethrow_exception(ptr);
    } catch (const TimeoutError&) {
        return FailureKind::timeout;
    } catch (const LivelockError&) {
        return FailureKind::livelock;
    } catch (const std::exception& e) {
        return std::string(e.what()) == "deadlock"
            ? FailureKind::deadlock
            : FailureKind::unexpected_exception;
    } catch (...) {
        return FailureKind::unexpected_exception;
    }
}

inline std::string capture_state_representation(const TestSpec& spec, void* object) {
    if (!spec.describe_concurrent || object == nullptr) return {};
    try {
        return spec.describe_concurrent(object);
    } catch (...) {
        return "<state representation threw: " + exception_message(std::current_exception()) + ">";
    }
}

inline std::optional<std::string> validate_concurrent_object(const TestSpec& spec, void* object) {
    if (!spec.validate_concurrent || object == nullptr) return std::nullopt;
    auto message = spec.validate_concurrent(object);
    if (message.empty()) return std::nullopt;
    return message;
}

inline std::string format_state_section(const std::string& state) {
    if (state.empty()) return {};
    return "state:\n  " + state + "\n";
}

inline std::string format_trace_lines(const char* title, const std::vector<std::string>& lines) {
    if (lines.empty()) return {};
    std::ostringstream out;
    out << title << ":\n";
    for (const auto& line : lines) out << "  " << line << "\n";
    return out.str();
}

inline std::string format_warnings_section(const std::vector<std::string>& warnings) {
    return format_trace_lines("warnings", warnings);
}

inline std::string format_trace_events_section(
    const std::vector<TraceEventRecord>& events,
    const TraceFilter* trace_filter = nullptr
) {
    if (events.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "trace events:\n";
    for (const auto& event : events) {
        auto line = to_string(event);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_memory_events_section(
    const std::vector<MemoryEventRecord>& events,
    const TraceFilter* trace_filter = nullptr
) {
    if (events.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "memory events:\n";
    for (const auto& event : events) {
        auto line = to_string(event);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_stm_events_section(
    const std::vector<StmEventRecord>& events,
    const TraceFilter* trace_filter = nullptr
) {
    if (events.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "stm events:\n";
    for (const auto& event : events) {
        auto line = to_string(event);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_source_accesses_section(
    const std::vector<SourceAccessEventRecord>& accesses,
    const TraceFilter* trace_filter = nullptr
) {
    if (accesses.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "source accesses:\n";
    for (const auto& access : accesses) {
        auto line = to_string(access);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_synchronization_events_section(
    const std::vector<SynchronizationEventRecord>& events,
    const TraceFilter* trace_filter = nullptr
) {
    if (events.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "synchronization events:\n";
    for (const auto& event : events) {
        auto line = to_string(event);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_event_dependencies_section(
    const EventDependencyGraph& graph,
    const TraceFilter* trace_filter = nullptr
) {
    if (graph.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "event dependencies:\n";
    for (const auto& node : graph.nodes) {
        auto line = "node " + to_string(node);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    for (const auto& edge : graph.edges) {
        auto line = "edge " + to_string(edge);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_event_dependency_analysis_section(
    const EventDependencyAnalysis& analysis,
    const TraceFilter* trace_filter = nullptr
) {
    if (analysis.explanation.empty() && analysis.topological_order.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "event dependency analysis:\n";
    auto emit_line = [&](std::string line) {
        if (trace_filter != nullptr && !trace_filter->accepts(line)) return;
        out << "  " << line << "\n";
        ++emitted;
    };
    emit_line(std::string("consistent=") + (analysis.consistent ? "true" : "false"));
    if (!analysis.explanation.empty()) {
        emit_line("explanation=" + analysis.explanation);
    }
    if (!analysis.topological_order.empty()) {
        std::ostringstream order;
        order << "topological_order=";
        for (std::size_t i = 0; i < analysis.topological_order.size(); ++i) {
            if (i != 0) order << ",";
            order << "#" << analysis.topological_order[i];
        }
        emit_line(order.str());
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_operation_dependency_footprints_section(
    const std::vector<OperationDependencyFootprint>& footprints,
    const TraceFilter* trace_filter = nullptr
) {
    if (footprints.empty()) return {};
    std::ostringstream out;
    int emitted = 0;
    out << "operation dependency footprints:\n";
    for (const auto& footprint : footprints) {
        auto line = to_string(footprint);
        if (trace_filter != nullptr && !trace_filter->accepts(line)) continue;
        out << "  " << line << "\n";
        ++emitted;
    }
    if (emitted == 0) return {};
    return out.str();
}

inline std::string format_failure_summary_section(FailureKind failure, const std::string& message) {
    if (failure == FailureKind::none && message.empty()) return {};
    std::ostringstream out;
    out << "failure:\n";
    out << "  kind: " << failure_kind_name(failure) << "\n";
    if (!message.empty()) {
        out << "  message: " << message << "\n";
    }
    return out.str();
}

inline std::string format_verifier_explanation_section(const std::string& explanation) {
    if (explanation.empty()) return {};
    std::ostringstream out;
    out << "verifier explanation:\n";
    std::istringstream input(explanation);
    std::string line;
    while (std::getline(input, line)) {
        out << "  " << line << "\n";
    }
    return out.str();
}

class DeadlineRuntime final : public Runtime {
public:
    DeadlineRuntime(
        std::chrono::steady_clock::time_point deadline,
        int thread_id,
        std::vector<std::string>& trace,
        std::mutex& trace_mutex,
        std::vector<std::string>& warnings,
        std::mutex& warnings_mutex,
        std::mutex& event_order_mutex,
        std::size_t& event_sequence,
        std::vector<TraceEventRecord>& trace_events,
        std::mutex& trace_events_mutex,
        std::size_t& trace_event_sequence,
        std::vector<MemoryEventRecord>& memory_events,
        std::mutex& memory_events_mutex,
        std::size_t& memory_event_sequence,
        std::vector<StmEventRecord>& stm_events,
        std::mutex& stm_events_mutex,
        std::size_t& stm_event_sequence,
        std::vector<SourceAccessEventRecord>& source_accesses,
        std::mutex& source_accesses_mutex,
        std::size_t& source_access_sequence,
        std::vector<SynchronizationEventRecord>& synchronization_events,
        std::mutex& synchronization_events_mutex,
        std::size_t& synchronization_event_sequence,
        TraceFilter trace_filter = {}
    ) : deadline_(deadline),
        thread_id_(thread_id),
        trace_(trace),
        trace_mutex_(trace_mutex),
        warnings_(warnings),
        warnings_mutex_(warnings_mutex),
        event_order_mutex_(event_order_mutex),
        event_sequence_(event_sequence),
        trace_events_(trace_events),
        trace_events_mutex_(trace_events_mutex),
        trace_event_sequence_(trace_event_sequence),
        memory_events_(memory_events),
        memory_events_mutex_(memory_events_mutex),
        memory_event_sequence_(memory_event_sequence),
        stm_events_(stm_events),
        stm_events_mutex_(stm_events_mutex),
        stm_event_sequence_(stm_event_sequence),
        source_accesses_(source_accesses),
        source_accesses_mutex_(source_accesses_mutex),
        source_access_sequence_(source_access_sequence),
        synchronization_events_(synchronization_events),
        synchronization_events_mutex_(synchronization_events_mutex),
        synchronization_event_sequence_(synchronization_event_sequence),
        trace_filter_(std::move(trace_filter)) {}

    void switch_point(const char* location) override {
        auto description = "switch-point " + std::string(location);
        record_trace_event("switch-point", description);
        append(std::move(description));
        check_timeout();
    }

    void event(const std::string& description) override {
        record_trace_event(detail::trace_event_kind(description), description);
        append(description);
        check_timeout();
    }

    void warning(const std::string& message) override {
        std::lock_guard lock(warnings_mutex_);
        detail::append_unique_warning(warnings_, message);
    }

    int thread_id() const override {
        return thread_id_;
    }

    void operation_begin(const OperationContext& context) override {
        active_operation_ = context;
        active_operation_->thread_id = thread_id_;
    }

    void operation_end(const OperationContext& context) override {
        if (
            active_operation_ &&
            active_operation_->actor_index == context.actor_index &&
            active_operation_->operation_index == context.operation_index
        ) {
            active_operation_.reset();
        }
    }

    void memory_event(const MemoryEvent& event) override {
        std::lock_guard order_lock(event_order_mutex_);
        std::lock_guard lock(memory_events_mutex_);
        memory_events_.push_back(detail::make_memory_event_record(
            memory_event_sequence_++,
            thread_id_,
            event,
            event_sequence_++,
            active_operation_ ? &*active_operation_ : nullptr
        ));
    }

    void stm_event(const stm::Event& event) override {
        {
            std::lock_guard order_lock(event_order_mutex_);
            std::lock_guard lock(stm_events_mutex_);
            stm_events_.push_back(detail::make_stm_event_record(
                stm_event_sequence_++,
                thread_id_,
                event,
                event_sequence_++,
                active_operation_ ? &*active_operation_ : nullptr
            ));
        }
        Runtime::stm_event(event);
    }

    void source_access_event(const SourceAccessEvent& event) override {
        std::lock_guard order_lock(event_order_mutex_);
        std::lock_guard lock(source_accesses_mutex_);
        source_accesses_.push_back(
            detail::make_source_access_event_record(
                source_access_sequence_++,
                thread_id_,
                event,
                event_sequence_++,
                active_operation_ ? &*active_operation_ : nullptr
            )
        );
    }

    void synchronization_event(const SynchronizationEvent& event) override {
        std::lock_guard order_lock(event_order_mutex_);
        std::lock_guard lock(synchronization_events_mutex_);
        synchronization_events_.push_back(
            detail::make_synchronization_event_record(
                synchronization_event_sequence_++,
                thread_id_,
                event,
                event_sequence_++,
                active_operation_ ? &*active_operation_ : nullptr
            )
        );
    }

private:
    void check_timeout() const {
        if (std::chrono::steady_clock::now() >= deadline_) {
            throw TimeoutError("timeout");
        }
    }

    void record_trace_event(std::string kind, std::string description) {
        std::lock_guard order_lock(event_order_mutex_);
        std::lock_guard lock(trace_events_mutex_);
        trace_events_.push_back(detail::make_trace_event_record(
            trace_event_sequence_++,
            thread_id_,
            std::move(kind),
            std::move(description),
            event_sequence_++,
            active_operation_ ? &*active_operation_ : nullptr
        ));
    }

    void append(std::string line) const {
        std::lock_guard lock(trace_mutex_);
        if (thread_id_ >= 0) {
            line = "thread " + std::to_string(thread_id_) + " " + line;
        }
        if (!trace_filter_.accepts(line)) return;
        if (trace_.size() < max_trace_lines_) {
            trace_.push_back(std::move(line));
        } else if (!trace_truncated_) {
            trace_.push_back("trace truncated");
            trace_truncated_ = true;
        }
    }

    std::chrono::steady_clock::time_point deadline_;
    int thread_id_;
    std::vector<std::string>& trace_;
    std::mutex& trace_mutex_;
    std::vector<std::string>& warnings_;
    std::mutex& warnings_mutex_;
    std::mutex& event_order_mutex_;
    std::size_t& event_sequence_;
    std::vector<TraceEventRecord>& trace_events_;
    std::mutex& trace_events_mutex_;
    std::size_t& trace_event_sequence_;
    std::vector<MemoryEventRecord>& memory_events_;
    std::mutex& memory_events_mutex_;
    std::size_t& memory_event_sequence_;
    std::vector<StmEventRecord>& stm_events_;
    std::mutex& stm_events_mutex_;
    std::size_t& stm_event_sequence_;
    std::vector<SourceAccessEventRecord>& source_accesses_;
    std::mutex& source_accesses_mutex_;
    std::size_t& source_access_sequence_;
    std::vector<SynchronizationEventRecord>& synchronization_events_;
    std::mutex& synchronization_events_mutex_;
    std::size_t& synchronization_event_sequence_;
    TraceFilter trace_filter_;
    std::optional<OperationContext> active_operation_;
    mutable bool trace_truncated_ = false;
    static constexpr std::size_t max_trace_lines_ = 200;
};

class ConcurrentBlockRuntime final : public Runtime {
public:
    explicit ConcurrentBlockRuntime(TraceFilter trace_filter = {})
        : trace_filter_(std::move(trace_filter)) {
        (void)thread_id();
    }

    void switch_point(const char* location) override {
        append("switch-point " + std::string(location));
    }

    void event(const std::string& description) override {
        append(description);
    }

    void warning(const std::string& message) override {
        std::lock_guard lock(mutex_);
        detail::append_unique_warning(warnings_, message);
    }

    int thread_id() const override {
        std::lock_guard lock(mutex_);
        return id_for_thread_locked(std::this_thread::get_id());
    }

    std::string trace_string() const {
        std::lock_guard lock(mutex_);
        std::ostringstream out;
        out << "concurrent test trace:\n";
        for (const auto& line : trace_) out << line << "\n";
        return out.str();
    }

    std::vector<std::string> warnings() const {
        std::lock_guard lock(mutex_);
        return warnings_;
    }

    std::vector<TraceEventRecord> trace_events() const {
        std::lock_guard lock(mutex_);
        return trace_events_;
    }

    std::vector<MemoryEventRecord> memory_events() const {
        std::lock_guard lock(mutex_);
        return memory_events_;
    }

    std::vector<StmEventRecord> stm_events() const {
        std::lock_guard lock(mutex_);
        return stm_events_;
    }

    std::vector<SourceAccessEventRecord> source_accesses() const {
        std::lock_guard lock(mutex_);
        return source_accesses_;
    }

    std::vector<SynchronizationEventRecord> synchronization_events() const {
        std::lock_guard lock(mutex_);
        return synchronization_events_;
    }

    void memory_event(const MemoryEvent& event) override {
        std::lock_guard lock(mutex_);
        memory_events_.push_back(detail::make_memory_event_record(
            memory_events_.size(),
            id_for_thread_locked(std::this_thread::get_id()),
            event,
            event_sequence_++
        ));
    }

    void stm_event(const stm::Event& event) override {
        {
            std::lock_guard lock(mutex_);
            stm_events_.push_back(detail::make_stm_event_record(
                stm_events_.size(),
                id_for_thread_locked(std::this_thread::get_id()),
                event,
                event_sequence_++
            ));
        }
        Runtime::stm_event(event);
    }

    void source_access_event(const SourceAccessEvent& event) override {
        std::lock_guard lock(mutex_);
        source_accesses_.push_back(detail::make_source_access_event_record(
            source_accesses_.size(),
            id_for_thread_locked(std::this_thread::get_id()),
            event,
            event_sequence_++
        ));
    }

    void synchronization_event(const SynchronizationEvent& event) override {
        std::lock_guard lock(mutex_);
        synchronization_events_.push_back(detail::make_synchronization_event_record(
            synchronization_events_.size(),
            id_for_thread_locked(std::this_thread::get_id()),
            event,
            event_sequence_++
        ));
    }

private:
    int id_for_thread_locked(std::thread::id id) const {
        auto it = thread_ids_.find(id);
        if (it != thread_ids_.end()) return it->second;
        const int assigned = next_thread_id_++;
        thread_ids_.emplace(id, assigned);
        return assigned;
    }

    void append(std::string line) const {
        std::lock_guard lock(mutex_);
        const int id = id_for_thread_locked(std::this_thread::get_id());
        trace_events_.push_back(detail::make_trace_event_record(
            trace_events_.size(),
            id,
            detail::trace_event_kind(line),
            line,
            event_sequence_++
        ));
        auto full_line = "thread " + std::to_string(id) + " " + std::move(line);
        if (trace_filter_.accepts(full_line)) {
            trace_.push_back(std::move(full_line));
        }
    }

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::thread::id, int> thread_ids_;
    mutable int next_thread_id_ = 0;
    mutable std::vector<std::string> trace_;
    mutable std::size_t event_sequence_ = 0;
    std::vector<std::string> warnings_;
    mutable std::vector<TraceEventRecord> trace_events_;
    std::vector<MemoryEventRecord> memory_events_;
    std::vector<StmEventRecord> stm_events_;
    std::vector<SourceAccessEventRecord> source_accesses_;
    std::vector<SynchronizationEventRecord> synchronization_events_;
    TraceFilter trace_filter_;
};

inline Value run_concurrent_actor(void* object, const TestSpec& spec, const Actor& actor) {
    const auto& op = spec.operations.at(actor.operation_index);
    return op.run_concurrent(object, actor.arguments);
}

inline std::vector<Value> run_concurrent_sequence(
    void* object,
    const TestSpec& spec,
    const std::vector<Actor>& actors,
    ClockSource& clock,
    int thread_id,
    std::vector<OperationInterval>* intervals
) {
    std::vector<Value> results;
    results.reserve(actors.size());
    if (intervals) {
        intervals->clear();
        intervals->reserve(actors.size());
    }
    for (std::size_t i = 0; i < actors.size(); ++i) {
        const auto& operation = spec.operations.at(actors[i].operation_index);
        detail::ScopedOperationContext operation_context(
            detail::make_operation_context(thread_id, i, actors[i], operation)
        );
        const auto invocation_clock = clock.now();
        trace_event(
            "operation.start thread=" + std::to_string(thread_id) +
            " actor=" + std::to_string(i) +
            " " + actors[i].to_string()
        );
        Value value;
        try {
            value = run_concurrent_actor(object, spec, actors[i]);
        } catch (...) {
            const auto exception = std::current_exception();
            const auto exception_value = detail::exception_result_value(exception);
            try {
                trace_event(
                    "operation.throw thread=" + std::to_string(thread_id) +
                    " actor=" + std::to_string(i) +
                    " " + actors[i].to_string() +
                    " -> " + exception_value.to_string()
                );
            } catch (...) {
            }
            const auto& op = spec.operations.at(actors[i].operation_index);
            if (op.options.exception_results) {
                value = exception_value;
            } else {
                const auto response_clock = clock.now();
                if (intervals) {
                    intervals->push_back(OperationInterval{
                        .thread_id = thread_id,
                        .actor_index = i,
                        .invocation_clock = invocation_clock,
                        .response_clock = response_clock,
                        .result = exception_value
                    });
                }
                operation_context.close();
                throw;
            }
        }
        const auto response_clock = clock.now();
        trace_event(
            "operation.finish thread=" + std::to_string(thread_id) +
            " actor=" + std::to_string(i) +
            " " + actors[i].to_string() +
            " -> " + value.to_string()
        );
        results.push_back(value);
        if (intervals) {
            intervals->push_back(OperationInterval{
                .thread_id = thread_id,
                .actor_index = i,
                .invocation_clock = invocation_clock,
                .response_clock = response_clock,
                .result = value
            });
        }
        operation_context.close();
    }
    return results;
}

inline std::vector<Value> run_concurrent_sequence(void* object, const TestSpec& spec, const std::vector<Actor>& actors) {
    AtomicClockSource clock;
    std::vector<OperationInterval> ignored;
    return run_concurrent_sequence(object, spec, actors, clock, -1, &ignored);
}

inline bool still_valid_scenario(const ExecutionScenario& scenario) {
    return scenario.valid();
}

inline std::string format_operation_intervals(const ExecutionScenario& scenario, const ExecutionResult& result) {
    std::ostringstream out;
    out << "operation clocks:\n";

    auto print_section = [&](const char* name, const std::vector<Actor>& actors, const std::vector<OperationInterval>& intervals) {
        for (std::size_t i = 0; i < actors.size() && i < intervals.size(); ++i) {
            const auto& interval = intervals[i];
            out << "  " << name << " " << i << " "
                << actors[i].to_string()
                << " -> " << interval.result.to_string()
                << " [" << interval.invocation_clock << ", " << interval.response_clock << "]\n";
        }
    };

    print_section("init", scenario.init, result.init_intervals);
    for (std::size_t t = 0; t < scenario.parallel.size() && t < result.parallel_intervals.size(); ++t) {
        for (std::size_t i = 0; i < scenario.parallel[t].size() && i < result.parallel_intervals[t].size(); ++i) {
            const auto& interval = result.parallel_intervals[t][i];
            out << "  thread " << t << " actor " << i << " "
                << scenario.parallel[t][i].to_string()
                << " -> " << interval.result.to_string()
                << " [" << interval.invocation_clock << ", " << interval.response_clock << "]\n";
        }
    }
    print_section("post", scenario.post, result.post_intervals);

    return out.str();
}

inline std::string format_interleaving_section(const ExecutionScenario& scenario, const ExecutionResult& result) {
    struct Row {
        std::uint64_t invocation_clock = 0;
        std::uint64_t response_clock = 0;
        int section_order = 0;
        std::string label;
        std::string actor;
        std::string response;
    };

    std::vector<Row> rows;

    auto add_rows = [&](int section_order, const std::string& label, const std::vector<Actor>& actors, const std::vector<OperationInterval>& intervals) {
        for (std::size_t i = 0; i < actors.size() && i < intervals.size(); ++i) {
            const auto& interval = intervals[i];
            rows.push_back(Row{
                .invocation_clock = interval.invocation_clock,
                .response_clock = interval.response_clock,
                .section_order = section_order,
                .label = label + " " + std::to_string(i),
                .actor = actors[i].to_string(),
                .response = interval.result.to_string()
            });
        }
    };

    add_rows(0, "init", scenario.init, result.init_intervals);
    for (std::size_t t = 0; t < scenario.parallel.size() && t < result.parallel_intervals.size(); ++t) {
        add_rows(
            1,
            "thread " + std::to_string(t) + " actor",
            scenario.parallel[t],
            result.parallel_intervals[t]
        );
    }
    add_rows(2, "post", scenario.post, result.post_intervals);

    if (rows.empty()) return {};

    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.invocation_clock != right.invocation_clock) return left.invocation_clock < right.invocation_clock;
        if (left.response_clock != right.response_clock) return left.response_clock < right.response_clock;
        return left.section_order < right.section_order;
    });

    std::ostringstream out;
    out << "interleaving:\n";
    for (const auto& row : rows) {
        out << "  [" << row.invocation_clock << ", " << row.response_clock << "] "
            << row.label << " " << row.actor << " -> " << row.response << "\n";
    }
    return out.str();
}

inline std::string format_thread_interleaving_section(const ExecutionScenario& scenario, const ExecutionResult& result) {
    if (result.init_intervals.empty() &&
        result.parallel_intervals.empty() &&
        result.post_intervals.empty()) {
        return {};
    }

    std::ostringstream out;
    out << "thread interleaving:\n";

    auto print_section = [&](const std::string& label, const std::vector<Actor>& actors, const std::vector<OperationInterval>& intervals) {
        out << "  " << label << ":\n";
        for (std::size_t i = 0; i < actors.size() && i < intervals.size(); ++i) {
            const auto& interval = intervals[i];
            out << "    actor " << i << " "
                << actors[i].to_string()
                << " -> " << interval.result.to_string()
                << " [" << interval.invocation_clock << ", " << interval.response_clock << "]\n";
        }
    };

    if (!result.init_intervals.empty()) {
        print_section("init", scenario.init, result.init_intervals);
    }

    for (std::size_t t = 0; t < scenario.parallel.size() && t < result.parallel_intervals.size(); ++t) {
        print_section("thread " + std::to_string(t), scenario.parallel[t], result.parallel_intervals[t]);
    }

    if (!result.post_intervals.empty()) {
        print_section("post", scenario.post, result.post_intervals);
    }

    return out.str();
}

inline std::string format_scenario_section(const ExecutionScenario& scenario) {
    return "\n" + scenario.to_string() + "\n";
}

inline std::string format_model_check_failure_sections(
    const ExecutionScenario& scenario,
    const ExecutionResult& execution_result,
    const std::vector<std::string>& warnings,
    const std::string& state_representation,
    const std::string& verifier_explanation = {},
    const std::vector<TraceEventRecord>* trace_events = nullptr,
    const std::vector<MemoryEventRecord>* memory_events = nullptr,
    const TraceFilter* trace_filter = nullptr,
    const std::vector<StmEventRecord>* stm_events = nullptr,
    const std::vector<SourceAccessEventRecord>* source_accesses = nullptr,
    const std::vector<SynchronizationEventRecord>* synchronization_events = nullptr,
    const EventDependencyGraph* event_dependencies = nullptr,
    const EventDependencyAnalysis* event_dependency_analysis = nullptr
) {
    return
        format_warnings_section(warnings) +
        (trace_events == nullptr ? std::string{} : format_trace_events_section(*trace_events, trace_filter)) +
        (memory_events == nullptr ? std::string{} : format_memory_events_section(*memory_events, trace_filter)) +
        (stm_events == nullptr ? std::string{} : format_stm_events_section(*stm_events, trace_filter)) +
        (source_accesses == nullptr ? std::string{} : format_source_accesses_section(*source_accesses, trace_filter)) +
        (synchronization_events == nullptr ? std::string{} : format_synchronization_events_section(*synchronization_events, trace_filter)) +
        (event_dependencies == nullptr ? std::string{} : format_event_dependencies_section(*event_dependencies, trace_filter)) +
        (event_dependency_analysis == nullptr ? std::string{} : format_event_dependency_analysis_section(*event_dependency_analysis, trace_filter)) +
        (event_dependencies == nullptr
            ? std::string{}
            : format_operation_dependency_footprints_section(
                build_operation_dependency_footprints(*event_dependencies),
                trace_filter
            )) +
        format_verifier_explanation_section(verifier_explanation) +
        format_interleaving_section(scenario, execution_result) +
        format_thread_interleaving_section(scenario, execution_result) +
        format_operation_intervals(scenario, execution_result) +
        format_state_section(state_representation) +
        format_scenario_section(scenario);
}

inline int count_schedule_context_switches(const std::vector<int>& schedule) {
    int switches = 0;
    for (std::size_t i = 1; i < schedule.size(); ++i) {
        if (schedule[i] != schedule[i - 1]) ++switches;
    }
    return switches;
}

inline bool has_model_checking_stats(const CheckResult& result) {
    return result.stats.schedules_generated != 0 ||
        result.stats.schedules_explored != 0 ||
        result.stats.schedules_pruned_by_context_bound != 0 ||
        result.stats.schedules_pruned_by_invocation_budget != 0 ||
        result.stats.schedules_pruned_by_operation_context != 0 ||
        result.stats.schedules_pruned_by_event_dependency != 0 ||
        result.stats.verifications_pruned_by_duplicate_history != 0 ||
        result.stats.context_switch_depth_increases != 0 ||
        result.stats.max_context_switch_depth_explored != 0 ||
        !result.schedule.empty() ||
        !result.schedule_decisions.empty();
}

inline std::string format_model_checking_stats_section(const CheckResult& result) {
    if (!has_model_checking_stats(result)) return {};

    std::ostringstream out;
    out << "model-checking stats:\n"
        << "  scenarios_generated=" << result.stats.scenarios_generated << "\n"
        << "  schedules_explored=" << result.stats.schedules_explored << "\n"
        << "  schedules_generated=" << result.stats.schedules_generated << "\n"
        << "  schedules_pruned_by_context_bound=" << result.stats.schedules_pruned_by_context_bound << "\n"
        << "  schedules_pruned_by_invocation_budget=" << result.stats.schedules_pruned_by_invocation_budget << "\n"
        << "  schedules_pruned_by_operation_context=" << result.stats.schedules_pruned_by_operation_context << "\n"
        << "  schedules_pruned_by_event_dependency=" << result.stats.schedules_pruned_by_event_dependency << "\n"
        << "  verifications_pruned_by_duplicate_history="
        << result.stats.verifications_pruned_by_duplicate_history << "\n"
        << "  context_switch_depth_increases=" << result.stats.context_switch_depth_increases << "\n"
        << "  max_context_switch_depth_explored=" << result.stats.max_context_switch_depth_explored << "\n"
        << "  retained_schedule_length=" << result.schedule.size() << "\n"
        << "  retained_schedule_context_switches=" << count_schedule_context_switches(result.schedule) << "\n"
        << "  retained_schedule_decisions=" << result.schedule_decisions.size() << "\n";
    return out.str();
}

inline void append_model_checking_stats_section(CheckResult& result) {
    if (result.trace.find("model-checking stats:\n") != std::string::npos) return;

    const auto stats_section = format_model_checking_stats_section(result);
    if (stats_section.empty()) return;
    if (!result.trace.empty() && result.trace.back() != '\n') result.trace += "\n";
    result.trace += stats_section;
}

} // namespace detail

inline std::string format_model_checking_stats(const CheckResult& result) {
    return detail::format_model_checking_stats_section(result);
}

template <typename Fn>
CheckResult run_concurrent_test(Fn&& fn, TraceFilter trace_filter = {}) {
    CheckResult result;
    const auto active_trace_filter = trace_filter;
    detail::ConcurrentBlockRuntime runtime(std::move(trace_filter));
    ScopedRuntime scoped(&runtime);
    try {
        runtime.event("concurrent_test.start");
        std::forward<Fn>(fn)();
        runtime.event("concurrent_test.finish");
        result.warnings = runtime.warnings();
        result.trace_events = runtime.trace_events();
        result.memory_events = runtime.memory_events();
        result.stm_events = runtime.stm_events();
        result.source_accesses = runtime.source_accesses();
        result.synchronization_events = runtime.synchronization_events();
        detail::refresh_event_dependencies(result);
        result.trace = runtime.trace_string();
        return result;
    } catch (...) {
        const auto exception = std::current_exception();
        result.success = false;
        result.failure = detail::failure_kind_from_exception(exception);
        result.exception = exception;
        result.message = detail::exception_message(exception);
        result.warnings = runtime.warnings();
        result.trace_events = runtime.trace_events();
        result.memory_events = runtime.memory_events();
        result.stm_events = runtime.stm_events();
        result.source_accesses = runtime.source_accesses();
        result.synchronization_events = runtime.synchronization_events();
        detail::refresh_event_dependencies(result);
        result.trace =
            detail::format_failure_summary_section(result.failure, result.message) +
            detail::format_warnings_section(result.warnings) +
            detail::format_trace_events_section(result.trace_events, &active_trace_filter) +
            detail::format_memory_events_section(result.memory_events, &active_trace_filter) +
            detail::format_stm_events_section(result.stm_events, &active_trace_filter) +
            detail::format_source_accesses_section(result.source_accesses, &active_trace_filter) +
            detail::format_synchronization_events_section(result.synchronization_events, &active_trace_filter) +
            detail::format_event_dependencies_section(result.event_dependencies, &active_trace_filter) +
            detail::format_event_dependency_analysis_section(result.event_dependency_analysis, &active_trace_filter) +
            runtime.trace_string();
        return result;
    }
}

class StressOptions {
public:
    StressOptions& iterations(int value) { detail::require_positive_option("iterations", value); config_.iterations = value; return *this; }
    StressOptions& invocations_per_iteration(int value) { detail::require_positive_option("invocations_per_iteration", value); config_.invocations_per_iteration = value; return *this; }
    StressOptions& threads(int value) { detail::require_positive_option("threads", value); config_.threads = value; return *this; }
    StressOptions& actors_per_thread(int value) { detail::require_positive_option("actors_per_thread", value); config_.actors_per_thread = value; return *this; }
    StressOptions& actors_before(int value) { detail::require_non_negative_option("actors_before", value); config_.actors_before = value; return *this; }
    StressOptions& actors_after(int value) { detail::require_non_negative_option("actors_after", value); config_.actors_after = value; return *this; }
    StressOptions& seed(std::uint64_t value) { config_.seed = value; return *this; }
    StressOptions& clock_source(ClockSourceKind value) { config_.clock_source = value; return *this; }
    StressOptions& memory_model(MemoryModel value) { detail::require_supported_memory_model(value); config_.memory_model = value; return *this; }
    StressOptions& invocation_timeout(std::chrono::milliseconds value) {
        if (value.count() < 0) {
            throw std::invalid_argument("lincheck option invocation_timeout must be >= 0ms");
        }
        config_.invocation_timeout = value;
        return *this;
    }
    StressOptions& trace_filter(TraceFilter value) { config_.trace_filter = std::move(value); return *this; }
    StressOptions& trace_include(std::string pattern) { config_.trace_filter.include(std::move(pattern)); return *this; }
    StressOptions& trace_exclude(std::string pattern) { config_.trace_filter.exclude(std::move(pattern)); return *this; }

    CheckResult check(std::string name, const TestSpec& spec) const {
        auto result = check(spec);
        detail::annotate_named_check(result, name);
        return result;
    }

    template <typename Builder>
        requires std::is_invocable_v<Builder&&>
    CheckResult check(std::string name, Builder&& builder) const {
        return check(std::move(name), detail::build_test_spec(std::forward<Builder>(builder)));
    }

    CheckResult check(std::string name, const TestSpec& spec, const ExecutionScenario& scenario) const {
        auto result = check(spec, scenario);
        detail::annotate_named_check(result, name);
        return result;
    }

    CheckResult check(const TestSpec& spec, const ExecutionScenario& scenario) const {
        if (!scenario.valid()) {
            throw std::invalid_argument("lincheck scenario must contain at least one parallel actor");
        }
        detail::validate_scenario_against_spec(spec, scenario);

        CheckResult success;
        success.scenario = scenario;
        success.stats.scenarios_generated = 1;
        success.warnings = make_clock_source(config_.clock_source)->warnings();
        LinearizabilityVerifier verifier(spec);
        for (int i = 0; i < config_.iterations; ++i) {
            for (int invocation = 0; invocation < config_.invocations_per_iteration; ++invocation) {
                auto result = run_invocation(spec, scenario);
                result.stats.scenarios_generated = 1;
                detail::append_unique_warnings(success.warnings, result.warnings);
                if (result.warnings.empty()) result.warnings = success.warnings;
                if (!result.success) return result;
                auto verification = verifier.verify_with_report(scenario, result.execution_result);
                if (!verification.success) {
                    result.success = false;
                    result.failure = FailureKind::invalid_results;
                    result.message = "invalid execution results";
                    result.verifier_explanation = std::move(verification.explanation);
                    result.trace = "stress invocation found non-linearizable result\n" +
                        detail::format_failure_summary_section(result.failure, result.message) +
                        detail::format_model_check_failure_sections(
                            scenario,
                            result.execution_result,
                            result.warnings,
                            result.state_representation,
                            result.verifier_explanation,
                            &result.trace_events,
                            &result.memory_events,
                            &config_.trace_filter,
                            &result.stm_events,
                            &result.source_accesses,
                            &result.synchronization_events,
                            &result.event_dependencies,
                            &result.event_dependency_analysis
                        ) +
                        result.trace;
                    return result;
                }
                if (
                    success.trace_events.empty() &&
                    success.memory_events.empty() &&
                    success.stm_events.empty() &&
                    success.source_accesses.empty() &&
                    success.synchronization_events.empty() &&
                    (!result.trace_events.empty() ||
                     !result.memory_events.empty() || !result.stm_events.empty() ||
                     !result.source_accesses.empty() || !result.synchronization_events.empty())
                ) {
                    success.scenario = scenario;
                    success.execution_result = result.execution_result;
                    success.trace_events = result.trace_events;
                    success.memory_events = result.memory_events;
                    success.stm_events = result.stm_events;
                    success.source_accesses = result.source_accesses;
                    success.synchronization_events = result.synchronization_events;
                    success.event_dependencies = result.event_dependencies;
                    success.event_dependency_analysis = result.event_dependency_analysis;
                    success.operation_dependency_footprints = result.operation_dependency_footprints;
                }
            }
        }
        return success;
    }

    CheckResult check(const TestSpec& spec) const {
        CheckResult success;
        success.warnings = make_clock_source(config_.clock_source)->warnings();
        RandomExecutionGenerator generator(
            spec,
            config_.threads,
            config_.actors_per_thread,
            config_.actors_before,
            config_.actors_after,
            config_.seed
        );
        LinearizabilityVerifier verifier(spec);
        for (int i = 0; i < config_.iterations; ++i) {
            const auto scenario = generator.next();
            if (!scenario.valid()) continue;
            for (int invocation = 0; invocation < config_.invocations_per_iteration; ++invocation) {
                auto result = run_invocation(spec, scenario);
                detail::append_unique_warnings(success.warnings, result.warnings);
                if (result.warnings.empty()) result.warnings = success.warnings;
                if (!result.success) return result;
                auto verification = verifier.verify_with_report(scenario, result.execution_result);
                if (!verification.success) {
                    result.success = false;
                    result.failure = FailureKind::invalid_results;
                    result.message = "invalid execution results";
                    result.verifier_explanation = std::move(verification.explanation);
                    result.trace = "stress invocation found non-linearizable result\n" +
                        detail::format_failure_summary_section(result.failure, result.message) +
                        detail::format_model_check_failure_sections(
                            scenario,
                            result.execution_result,
                            result.warnings,
                            result.state_representation,
                            result.verifier_explanation,
                            &result.trace_events,
                            &result.memory_events,
                            &config_.trace_filter,
                            &result.stm_events,
                            &result.source_accesses,
                            &result.synchronization_events,
                            &result.event_dependencies,
                            &result.event_dependency_analysis
                        ) +
                        result.trace;
                    return result;
                }
                if (
                    success.trace_events.empty() &&
                    success.memory_events.empty() &&
                    success.stm_events.empty() &&
                    success.source_accesses.empty() &&
                    success.synchronization_events.empty() &&
                    (!result.trace_events.empty() ||
                     !result.memory_events.empty() || !result.stm_events.empty() ||
                     !result.source_accesses.empty() || !result.synchronization_events.empty())
                ) {
                    success.scenario = scenario;
                    success.execution_result = result.execution_result;
                    success.trace_events = result.trace_events;
                    success.memory_events = result.memory_events;
                    success.stm_events = result.stm_events;
                    success.source_accesses = result.source_accesses;
                    success.synchronization_events = result.synchronization_events;
                    success.event_dependencies = result.event_dependencies;
                    success.event_dependency_analysis = result.event_dependency_analysis;
                    success.operation_dependency_footprints = result.operation_dependency_footprints;
                }
            }
        }
        return success;
    }

private:
    CheckResult run_invocation(const TestSpec& spec, const ExecutionScenario& scenario) const {
        CheckResult result;
        result.scenario = scenario;
        auto clock = make_clock_source(config_.clock_source);
        result.warnings = clock->warnings();
        auto object = spec.make_concurrent();
        const bool timeout_enabled = config_.invocation_timeout.count() > 0;
        const auto deadline = timeout_enabled
            ? std::chrono::steady_clock::now() + config_.invocation_timeout
            : std::chrono::steady_clock::time_point::max();
        std::vector<std::string> runtime_trace;
        std::mutex runtime_trace_mutex;
        std::vector<std::string> runtime_warnings;
        std::mutex runtime_warnings_mutex;
        std::mutex runtime_events_mutex;
        std::size_t runtime_event_sequence = 0;
        std::mutex runtime_trace_events_mutex;
        std::size_t runtime_trace_event_sequence = 0;
        std::mutex runtime_memory_events_mutex;
        std::size_t runtime_memory_event_sequence = 0;
        std::mutex runtime_stm_events_mutex;
        std::size_t runtime_stm_event_sequence = 0;
        std::mutex runtime_source_accesses_mutex;
        std::size_t runtime_source_access_sequence = 0;
        std::mutex runtime_synchronization_events_mutex;
        std::size_t runtime_synchronization_event_sequence = 0;
        detail::VectorWarningSink warning_sink(runtime_warnings, runtime_warnings_mutex);

        auto append_runtime_trace = [&](std::string line) {
            std::lock_guard lock(runtime_trace_mutex);
            if (config_.trace_filter.accepts(line)) {
                runtime_trace.push_back(std::move(line));
            }
        };

        auto merge_runtime_warnings = [&] {
            std::vector<std::string> warnings;
            {
                std::lock_guard lock(runtime_warnings_mutex);
                warnings = runtime_warnings;
            }
            detail::append_unique_warnings(result.warnings, warnings);
        };

        auto run_sequence = [&](int thread_id, const std::vector<Actor>& actors, std::vector<OperationInterval>* intervals) {
            ScopedWarningSink warning_scope(&warning_sink);
            detail::DeadlineRuntime runtime(
                deadline,
                thread_id,
                runtime_trace,
                runtime_trace_mutex,
                runtime_warnings,
                runtime_warnings_mutex,
                runtime_events_mutex,
                runtime_event_sequence,
                result.trace_events,
                runtime_trace_events_mutex,
                runtime_trace_event_sequence,
                result.memory_events,
                runtime_memory_events_mutex,
                runtime_memory_event_sequence,
                result.stm_events,
                runtime_stm_events_mutex,
                runtime_stm_event_sequence,
                result.source_accesses,
                runtime_source_accesses_mutex,
                runtime_source_access_sequence,
                result.synchronization_events,
                runtime_synchronization_events_mutex,
                runtime_synchronization_event_sequence,
                config_.trace_filter
            );
            ScopedRuntime scoped(&runtime);
            return detail::run_concurrent_sequence(
                object.get(),
                spec,
                actors,
                *clock,
                thread_id,
                intervals
            );
        };

        auto format_runtime_trace = [&] {
            std::lock_guard lock(runtime_trace_mutex);
            return detail::format_trace_lines("stress runtime trace", runtime_trace);
        };

        auto timed_out = [&] {
            return timeout_enabled && std::chrono::steady_clock::now() >= deadline;
        };

        auto finish_exception = [&](const std::exception_ptr& exception) {
            merge_runtime_warnings();
            result.state_representation = detail::capture_state_representation(spec, object.get());
            result.success = false;
            result.failure = detail::failure_kind_from_exception(exception);
            result.exception = exception;
            result.message = detail::exception_message(exception);
            detail::refresh_event_dependencies(result);
            result.trace =
                detail::format_failure_summary_section(result.failure, result.message) +
                detail::format_model_check_failure_sections(
                    scenario,
                    result.execution_result,
                    result.warnings,
                    result.state_representation,
                    {},
                    &result.trace_events,
                    &result.memory_events,
                    &config_.trace_filter,
                    &result.stm_events,
                    &result.source_accesses,
                    &result.synchronization_events,
                    &result.event_dependencies,
                    &result.event_dependency_analysis
                ) +
                format_runtime_trace();
            return result;
        };

        auto finish_timeout = [&] {
            merge_runtime_warnings();
            result.state_representation = detail::capture_state_representation(spec, object.get());
            result.success = false;
            result.failure = FailureKind::timeout;
            result.message = "timeout";
            detail::refresh_event_dependencies(result);
            result.trace =
                detail::format_failure_summary_section(result.failure, result.message) +
                detail::format_model_check_failure_sections(
                    scenario,
                    result.execution_result,
                    result.warnings,
                    result.state_representation,
                    {},
                    &result.trace_events,
                    &result.memory_events,
                    &config_.trace_filter,
                    &result.stm_events,
                    &result.source_accesses,
                    &result.synchronization_events,
                    &result.event_dependencies,
                    &result.event_dependency_analysis
                ) +
                format_runtime_trace();
            return result;
        };

        auto finish_validation_failure = [&](std::string message) {
            merge_runtime_warnings();
            result.state_representation = detail::capture_state_representation(spec, object.get());
            result.success = false;
            result.failure = FailureKind::validation_failure;
            result.message = message.empty() ? "validation failed" : std::move(message);
            detail::refresh_event_dependencies(result);
            result.trace =
                detail::format_failure_summary_section(result.failure, result.message) +
                "validation failed: " + result.message + "\n" +
                detail::format_model_check_failure_sections(
                    scenario,
                    result.execution_result,
                    result.warnings,
                    result.state_representation,
                    {},
                    &result.trace_events,
                    &result.memory_events,
                    &config_.trace_filter,
                    &result.stm_events,
                    &result.source_accesses,
                    &result.synchronization_events,
                    &result.event_dependencies,
                    &result.event_dependency_analysis
                ) +
                format_runtime_trace();
            return result;
        };

        try {
            result.execution_result.init_results = run_sequence(-1, scenario.init, &result.execution_result.init_intervals);
            if (timed_out()) return finish_timeout();
            result.execution_result.parallel_results.resize(scenario.parallel.size());
            result.execution_result.parallel_intervals.resize(scenario.parallel.size());

            std::mutex start_mutex;
            std::condition_variable start_cv;
            bool start = false;
            std::size_t ready_workers = 0;
            std::vector<std::thread> workers;
            std::exception_ptr exception;
            std::mutex exception_mutex;

            for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
                workers.emplace_back([&, t] {
                    try {
                        {
                            std::unique_lock lock(start_mutex);
                            ++ready_workers;
                            append_runtime_trace("stress.worker.ready thread=" + std::to_string(t));
                            start_cv.notify_all();
                            start_cv.wait(lock, [&] { return start; });
                        }
                        result.execution_result.parallel_results[t] =
                            run_sequence(
                                static_cast<int>(t),
                                scenario.parallel[t],
                                &result.execution_result.parallel_intervals[t]
                            );
                    } catch (...) {
                        std::lock_guard lock(exception_mutex);
                        if (!exception) exception = std::current_exception();
                    }
                });
            }
            {
                std::unique_lock lock(start_mutex);
                start_cv.wait(lock, [&] {
                    return ready_workers == scenario.parallel.size();
                });
                append_runtime_trace("stress.parallel.start threads=" + std::to_string(scenario.parallel.size()));
                start = true;
            }
            start_cv.notify_all();
            for (auto& worker : workers) worker.join();
            if (exception) {
                return finish_exception(exception);
            }
            if (timed_out()) return finish_timeout();
            result.execution_result.post_results = run_sequence(-1, scenario.post, &result.execution_result.post_intervals);
            if (auto validation_error = detail::validate_concurrent_object(spec, object.get())) {
                return finish_validation_failure(*validation_error);
            }
            result.state_representation = detail::capture_state_representation(spec, object.get());
            if (timed_out()) return finish_timeout();
            merge_runtime_warnings();
            detail::refresh_event_dependencies(result);
            result.trace = format_runtime_trace();
            return result;
        } catch (...) {
            return finish_exception(std::current_exception());
        }
    }

    OptionsConfig config_;
};

class CooperativeScheduler final : public Runtime {
public:
    CooperativeScheduler(
        std::vector<int> schedule,
        std::size_t thread_count,
        int switch_budget = 10000,
        TraceFilter trace_filter = {}
    )
        : schedule_(std::move(schedule)),
        finished_(thread_count, false),
        ready_(thread_count, false),
        blocked_(thread_count, false),
        aborted_(thread_count, false),
        timed_condition_waiting_(thread_count, false),
        condition_timed_out_(thread_count, false),
        timed_lock_waiting_(thread_count, false),
        lock_timed_out_(thread_count, false),
        timed_semaphore_waiting_(thread_count, false),
        semaphore_timed_out_(thread_count, false),
        switch_budget_(std::max(1, switch_budget)),
        active_operations_(thread_count),
        trace_filter_(std::move(trace_filter)) {}

    void worker_ready(std::size_t thread_id) {
        std::unique_lock lock(mutex_);
        ready_[thread_id] = true;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                (started_ && scheduled_thread_ == static_cast<int>(thread_id));
        });
        check_abort_locked(static_cast<int>(thread_id));
    }

    void start() {
        std::lock_guard lock(mutex_);
        cv_.notify_all();
    }

    void wait_until_ready() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
            return std::all_of(ready_.begin(), ready_.end(), [](bool ready) { return ready; });
        });
        scheduled_thread_ = choose_initial_thread_locked();
        started_ = true;
        append_trace_locked("initial -> thread " + std::to_string(scheduled_thread_));
        cv_.notify_all();
    }

    void finish_thread(std::size_t thread_id) {
        std::unique_lock lock(mutex_);
        if (!finished_[thread_id]) {
            finished_[thread_id] = true;
            append_trace_locked("thread " + std::to_string(thread_id) + " finished");
        }
        if (scheduled_thread_ == static_cast<int>(thread_id)) {
            scheduled_thread_ = choose_next_active_or_timeout_locked(thread_id);
            if (scheduled_thread_ < 0) {
                mark_deadlock_if_unfinished_threads_blocked_locked();
            }
        }
        cv_.notify_all();
    }

    void request_abort(std::size_t source_thread) {
        std::lock_guard lock(mutex_);
        if (!abort_requested_) {
            append_trace_locked("abort requested by thread " + std::to_string(source_thread));
        }
        abort_requested_ = true;
        for (std::size_t i = 0; i < aborted_.size(); ++i) {
            if (i != source_thread && !finished_[i]) {
                aborted_[i] = true;
            }
        }
        cv_.notify_all();
    }

    void switch_point(const char* location) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        const auto description = "switch-point " + std::string(location);
        record_trace_event_locked(id, "switch-point", description);
        append_trace_locked("thread " + std::to_string(id) + " " + description);
        ++switch_count_;
        if (switch_count_ > switch_budget_) {
            append_trace_locked("livelock: switch-point budget exceeded");
            cv_.notify_all();
            throw LivelockError("livelock");
        }
        if (scheduled_thread_ != id) {
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    finished_[static_cast<std::size_t>(id)] ||
                    scheduled_thread_ == id;
            });
        }
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        if (finished_[static_cast<std::size_t>(id)]) return;

        const int next = choose_scheduled_target_locked(id, location);
        if (next != id && next >= 0) {
            append_trace_locked("switch thread " + std::to_string(id) + " -> " + std::to_string(next));
            scheduled_thread_ = next;
            cv_.notify_all();
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    finished_[static_cast<std::size_t>(id)] ||
                    scheduled_thread_ == id;
            });
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");
        }
    }

    void event(const std::string& description) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        record_trace_event_locked(id, detail::trace_event_kind(description), description);
        if (id >= 0) {
            append_trace_locked("thread " + std::to_string(id) + " " + description);
        } else {
            append_trace_locked(description);
        }
    }

    void operation_begin(const OperationContext& context) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::lock_guard lock(mutex_);
        if (static_cast<std::size_t>(id) >= active_operations_.size()) return;
        auto stored = context;
        stored.thread_id = id;
        active_operations_[static_cast<std::size_t>(id)] = std::move(stored);
    }

    void operation_end(const OperationContext& context) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::lock_guard lock(mutex_);
        if (static_cast<std::size_t>(id) >= active_operations_.size()) return;
        auto& active = active_operations_[static_cast<std::size_t>(id)];
        if (
            active &&
            active->actor_index == context.actor_index &&
            active->operation_index == context.operation_index
        ) {
            active.reset();
        }
    }

    void warning(const std::string& message) override {
        std::lock_guard lock(mutex_);
        detail::append_unique_warning(warnings_, message);
    }

    int thread_id() const override {
        return current_thread_id_;
    }

    bool manages_locks() const override {
        return true;
    }

    bool manages_parking() const override {
        return true;
    }

    bool manages_atomic_waits() const override {
        return true;
    }

    bool manages_semaphores() const override {
        return true;
    }

    bool manages_latches() const override {
        return true;
    }

    bool manages_barriers() const override {
        return true;
    }

    void lock(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        append_trace_locked("thread " + std::to_string(id) + " lock attempt " + pointer_string(address));
        while (true) {
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            auto& state = locks_[address];
            if (state.owner == id || (state.owner == -1 && state.shared_owners.empty())) {
                state.owner = id;
                ++state.depth;
                append_trace_locked("thread " + std::to_string(id) + " lock acquired " + pointer_string(address));
                return;
            }

            append_trace_locked(
                "thread " + std::to_string(id) + " blocked on lock " + pointer_string(address) +
                " owned by thread " + std::to_string(state.owner)
            );
            blocked_[static_cast<std::size_t>(id)] = true;
            if (std::find(state.waiters.begin(), state.waiters.end(), id) == state.waiters.end()) {
                state.waiters.push_back(id);
            }

            const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
            if (next < 0) {
                deadlocked_ = true;
                append_trace_locked("deadlock: all unfinished threads are blocked");
                cv_.notify_all();
                throw std::runtime_error("deadlock");
            }

            scheduled_thread_ = next;
            cv_.notify_all();
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
            });
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            auto resumed = locks_.find(address);
            if (resumed != locks_.end() && resumed->second.owner == id && resumed->second.depth > 0) {
                append_trace_locked("thread " + std::to_string(id) + " lock acquired " + pointer_string(address));
                return;
            }
        }
    }

    void unlock(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        release_lock_locked(address, id);
        cv_.notify_all();
    }

    bool try_lock(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& state = locks_[address];
        if (state.owner == id || (state.owner == -1 && state.shared_owners.empty())) {
            state.owner = id;
            ++state.depth;
            append_trace_locked("thread " + std::to_string(id) + " try_lock acquired " + pointer_string(address));
            return true;
        }

        append_trace_locked(
            "thread " + std::to_string(id) + " try_lock failed " + pointer_string(address) +
            " owned by thread " + std::to_string(state.owner)
        );
        return false;
    }

    bool try_lock_for(const void* address, std::chrono::nanoseconds timeout) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& state = locks_[address];
        append_trace_locked(
            "thread " + std::to_string(id) + " try_lock_for " +
            pointer_string(address)
        );
        if (state.owner == id || (state.owner == -1 && state.shared_owners.empty())) {
            state.owner = id;
            ++state.depth;
            append_trace_locked(
                "thread " + std::to_string(id) + " timed lock acquired " +
                pointer_string(address)
            );
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            append_trace_locked(
                "thread " + std::to_string(id) + " timed lock expired " +
                pointer_string(address)
            );
            return false;
        }

        if (std::find(state.waiters.begin(), state.waiters.end(), id) == state.waiters.end()) {
            state.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        timed_lock_waiting_[static_cast<std::size_t>(id)] = true;
        lock_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) + " waiting with timeout on lock " +
            pointer_string(address) + " owned by thread " + std::to_string(state.owner)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            timeout_lock_waiter_locked(address, id);
        } else {
            scheduled_thread_ = next;
        }

        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        const bool timed_out = lock_timed_out_[static_cast<std::size_t>(id)];
        timed_lock_waiting_[static_cast<std::size_t>(id)] = false;
        lock_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) +
            (timed_out ? " timed lock expired on lock " : " timed lock woke on lock ") +
            pointer_string(address)
        );
        return !timed_out;
    }

    void lock_shared(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        append_trace_locked("thread " + std::to_string(id) + " shared lock attempt " + pointer_string(address));
        while (true) {
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            auto& state = locks_[address];
            if (state.owner == -1) {
                ++state.shared_owners[id];
                append_trace_locked("thread " + std::to_string(id) + " shared lock acquired " + pointer_string(address));
                return;
            }

            append_trace_locked(
                "thread " + std::to_string(id) + " blocked on shared lock " + pointer_string(address) +
                " owned by thread " + std::to_string(state.owner)
            );
            blocked_[static_cast<std::size_t>(id)] = true;
            if (std::find(state.shared_waiters.begin(), state.shared_waiters.end(), id) == state.shared_waiters.end()) {
                state.shared_waiters.push_back(id);
            }

            const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
            if (next < 0) {
                deadlocked_ = true;
                append_trace_locked("deadlock: all unfinished threads are blocked");
                cv_.notify_all();
                throw std::runtime_error("deadlock");
            }

            scheduled_thread_ = next;
            cv_.notify_all();
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
            });
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            auto resumed = locks_.find(address);
            if (
                resumed != locks_.end() &&
                resumed->second.shared_owners.find(id) != resumed->second.shared_owners.end()
            ) {
                append_trace_locked("thread " + std::to_string(id) + " shared lock acquired " + pointer_string(address));
                return;
            }
        }
    }

    void unlock_shared(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        release_shared_lock_locked(address, id);
        cv_.notify_all();
    }

    bool try_lock_shared(const void* address) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& state = locks_[address];
        if (state.owner == -1) {
            ++state.shared_owners[id];
            append_trace_locked("thread " + std::to_string(id) + " try_lock_shared acquired " + pointer_string(address));
            return true;
        }

        append_trace_locked(
            "thread " + std::to_string(id) + " try_lock_shared failed " + pointer_string(address) +
            " owned by thread " + std::to_string(state.owner)
        );
        return false;
    }

    bool try_lock_shared_for(const void* address, std::chrono::nanoseconds timeout) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& state = locks_[address];
        append_trace_locked(
            "thread " + std::to_string(id) + " try_lock_shared_for " +
            pointer_string(address)
        );
        if (state.owner == -1) {
            ++state.shared_owners[id];
            append_trace_locked(
                "thread " + std::to_string(id) + " timed shared lock acquired " +
                pointer_string(address)
            );
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            append_trace_locked(
                "thread " + std::to_string(id) + " timed shared lock expired " +
                pointer_string(address)
            );
            return false;
        }

        if (std::find(state.shared_waiters.begin(), state.shared_waiters.end(), id) == state.shared_waiters.end()) {
            state.shared_waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        timed_lock_waiting_[static_cast<std::size_t>(id)] = true;
        lock_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) + " waiting with timeout on shared lock " +
            pointer_string(address) + " owned by thread " + std::to_string(state.owner)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            timeout_lock_waiter_locked(address, id);
        } else {
            scheduled_thread_ = next;
        }

        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        const bool timed_out = lock_timed_out_[static_cast<std::size_t>(id)];
        timed_lock_waiting_[static_cast<std::size_t>(id)] = false;
        lock_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) +
            (timed_out ? " timed shared lock expired on lock " : " timed shared lock woke on lock ") +
            pointer_string(address)
        );
        return !timed_out;
    }

    bool owns_lock(const void* address) const override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::lock_guard lock(mutex_);
        auto it = locks_.find(address);
        return it != locks_.end() && it->second.owner == id && it->second.depth > 0;
    }

    bool owns_shared_lock(const void* address) const override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::lock_guard lock(mutex_);
        auto it = locks_.find(address);
        if (it == locks_.end()) return false;
        auto owner = it->second.shared_owners.find(id);
        return owner != it->second.shared_owners.end() && owner->second > 0;
    }

    void wait_condition(const void* condition_address, const void* lock_address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        {
            std::unique_lock lock(mutex_);
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");
            require_single_depth_lock_locked(lock_address, id);
            append_trace_locked(
                "thread " + std::to_string(id) + " waiting on condition_variable " +
                pointer_string(condition_address)
            );
            release_lock_locked(lock_address, id);

            auto& condition = conditions_[condition_address];
            if (std::find(condition.waiters.begin(), condition.waiters.end(), id) == condition.waiters.end()) {
                condition.waiters.push_back(id);
            }
            blocked_[static_cast<std::size_t>(id)] = true;

            const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
            if (next < 0) {
                deadlocked_ = true;
                append_trace_locked("deadlock: all unfinished threads are blocked");
                cv_.notify_all();
                throw std::runtime_error("deadlock");
            }

            scheduled_thread_ = next;
            cv_.notify_all();
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
            });
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            append_trace_locked(
                "thread " + std::to_string(id) + " woke on condition_variable " +
                pointer_string(condition_address)
            );
        }

        lock(lock_address);
    }

    std::cv_status wait_condition_for(
        const void* condition_address,
        const void* lock_address,
        std::chrono::nanoseconds timeout
    ) override {
        const auto id = current_thread_id_;
        if (id < 0) return std::cv_status::timeout;

        bool timed_out = false;
        {
            std::unique_lock lock(mutex_);
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");
            require_single_depth_lock_locked(lock_address, id);
            append_trace_locked(
                "thread " + std::to_string(id) + " waiting with timeout on condition_variable " +
                pointer_string(condition_address)
            );
            release_lock_locked(lock_address, id);

            auto& condition = conditions_[condition_address];
            if (std::find(condition.waiters.begin(), condition.waiters.end(), id) == condition.waiters.end()) {
                condition.waiters.push_back(id);
            }
            blocked_[static_cast<std::size_t>(id)] = true;
            timed_condition_waiting_[static_cast<std::size_t>(id)] = true;
            condition_timed_out_[static_cast<std::size_t>(id)] = false;

            if (timeout <= std::chrono::nanoseconds::zero()) {
                timeout_condition_waiter_locked(condition_address, id);
            } else {
                const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
                if (next < 0) {
                    timeout_condition_waiter_locked(condition_address, id);
                } else {
                    scheduled_thread_ = next;
                }
            }

            cv_.notify_all();
            cv_.wait(lock, [&] {
                return abort_requested_ ||
                    deadlocked_ ||
                    (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
            });
            check_abort_locked(id);
            if (deadlocked_) throw std::runtime_error("deadlock");

            timed_out = condition_timed_out_[static_cast<std::size_t>(id)];
            timed_condition_waiting_[static_cast<std::size_t>(id)] = false;
            condition_timed_out_[static_cast<std::size_t>(id)] = false;

            append_trace_locked(
                "thread " + std::to_string(id) +
                (timed_out ? " timed wait expired on condition_variable " : " timed wait notified on condition_variable ") +
                pointer_string(condition_address)
            );
        }

        lock(lock_address);
        return timed_out ? std::cv_status::timeout : std::cv_status::no_timeout;
    }

    void notify_one_condition(const void* condition_address) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        append_trace_locked(
            "thread " + std::to_string(id) + " notify_one condition_variable " +
            pointer_string(condition_address)
        );
        notify_one_condition_locked(condition_address);
        cv_.notify_all();
    }

    void notify_all_condition(const void* condition_address) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        append_trace_locked(
            "thread " + std::to_string(id) + " notify_all condition_variable " +
            pointer_string(condition_address)
        );
        auto& condition = conditions_[condition_address];
        if (condition.waiters.empty()) {
            append_trace_locked("condition_variable " + pointer_string(condition_address) + " notify_all had no waiters");
        }
        while (!condition.waiters.empty()) {
            notify_one_condition_locked(condition_address);
        }
        cv_.notify_all();
    }

    void wait_atomic(const void* atomic_address, std::string_view expected) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        const std::string expected_text(expected);
        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& atomic = atomic_waits_[atomic_address];
        append_trace_locked(
            "thread " + std::to_string(id) + " waiting on atomic " +
            pointer_string(atomic_address) + " expected=" + expected_text
        );
        if (std::find(atomic.waiters.begin(), atomic.waiters.end(), id) == atomic.waiters.end()) {
            atomic.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            deadlocked_ = true;
            append_trace_locked("deadlock: all unfinished threads are blocked");
            cv_.notify_all();
            throw std::runtime_error("deadlock");
        }

        scheduled_thread_ = next;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        append_trace_locked(
            "thread " + std::to_string(id) + " woke on atomic " +
            pointer_string(atomic_address) + " expected=" + expected_text
        );
    }

    void notify_one_atomic(const void* atomic_address) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        append_trace_locked(
            "thread " + std::to_string(id) + " notify_one atomic " +
            pointer_string(atomic_address)
        );
        notify_one_atomic_locked(atomic_address);
        cv_.notify_all();
    }

    void notify_all_atomic(const void* atomic_address) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        append_trace_locked(
            "thread " + std::to_string(id) + " notify_all atomic " +
            pointer_string(atomic_address)
        );
        auto& atomic = atomic_waits_[atomic_address];
        if (atomic.waiters.empty()) {
            append_trace_locked("atomic " + pointer_string(atomic_address) + " notify_all had no waiters");
        }
        while (!atomic.waiters.empty()) {
            notify_one_atomic_locked(atomic_address);
        }
        cv_.notify_all();
    }

    void acquire_semaphore(const void* semaphore_address, std::ptrdiff_t observed_permits) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& semaphore = semaphore_state_locked(semaphore_address, observed_permits);
        append_trace_locked(
            "thread " + std::to_string(id) + " acquire semaphore " +
            pointer_string(semaphore_address)
        );
        if (semaphore.permits > 0) {
            --semaphore.permits;
            append_trace_locked(
                "thread " + std::to_string(id) + " acquired semaphore " +
                pointer_string(semaphore_address)
            );
            return;
        }

        if (std::find(semaphore.waiters.begin(), semaphore.waiters.end(), id) == semaphore.waiters.end()) {
            semaphore.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        append_trace_locked(
            "thread " + std::to_string(id) + " blocked on semaphore " +
            pointer_string(semaphore_address)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            deadlocked_ = true;
            append_trace_locked("deadlock: all unfinished threads are blocked");
            cv_.notify_all();
            throw std::runtime_error("deadlock");
        }

        scheduled_thread_ = next;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        append_trace_locked(
            "thread " + std::to_string(id) + " woke on semaphore " +
            pointer_string(semaphore_address)
        );
    }

    bool try_acquire_semaphore(const void* semaphore_address, std::ptrdiff_t observed_permits) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& semaphore = semaphore_state_locked(semaphore_address, observed_permits);
        if (semaphore.permits > 0) {
            --semaphore.permits;
            append_trace_locked(
                "thread " + std::to_string(id) + " try_acquire semaphore succeeded " +
                pointer_string(semaphore_address)
            );
            return true;
        }

        append_trace_locked(
            "thread " + std::to_string(id) + " try_acquire semaphore failed " +
            pointer_string(semaphore_address)
        );
        return false;
    }

    bool try_acquire_semaphore_for(
        const void* semaphore_address,
        std::ptrdiff_t observed_permits,
        std::chrono::nanoseconds timeout
    ) override {
        const auto id = current_thread_id_;
        if (id < 0) return false;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& semaphore = semaphore_state_locked(semaphore_address, observed_permits);
        append_trace_locked(
            "thread " + std::to_string(id) + " try_acquire_for semaphore " +
            pointer_string(semaphore_address)
        );
        if (semaphore.permits > 0) {
            --semaphore.permits;
            append_trace_locked(
                "thread " + std::to_string(id) + " timed acquire semaphore succeeded " +
                pointer_string(semaphore_address)
            );
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            append_trace_locked(
                "thread " + std::to_string(id) + " timed acquire semaphore expired " +
                pointer_string(semaphore_address)
            );
            return false;
        }

        if (std::find(semaphore.waiters.begin(), semaphore.waiters.end(), id) == semaphore.waiters.end()) {
            semaphore.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        timed_semaphore_waiting_[static_cast<std::size_t>(id)] = true;
        semaphore_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) + " waiting with timeout on semaphore " +
            pointer_string(semaphore_address)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            timeout_semaphore_waiter_locked(semaphore_address, id);
        } else {
            scheduled_thread_ = next;
        }

        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        const bool timed_out = semaphore_timed_out_[static_cast<std::size_t>(id)];
        timed_semaphore_waiting_[static_cast<std::size_t>(id)] = false;
        semaphore_timed_out_[static_cast<std::size_t>(id)] = false;
        append_trace_locked(
            "thread " + std::to_string(id) +
            (timed_out ? " timed acquire expired on semaphore " : " timed acquire woke on semaphore ") +
            pointer_string(semaphore_address)
        );
        return !timed_out;
    }

    void release_semaphore(
        const void* semaphore_address,
        std::ptrdiff_t update,
        std::ptrdiff_t observed_permits
    ) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& semaphore = semaphore_state_locked(semaphore_address, observed_permits);
        append_trace_locked(
            "thread " + std::to_string(id) + " release semaphore " +
            pointer_string(semaphore_address) + " update=" + std::to_string(update)
        );
        release_semaphore_locked(semaphore_address, semaphore, update);
        cv_.notify_all();
    }

    void count_down_latch(
        const void* latch_address,
        std::ptrdiff_t update,
        std::ptrdiff_t observed_count
    ) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& latch = latch_state_locked(latch_address, observed_count);
        append_trace_locked(
            "thread " + std::to_string(id) + " count_down latch " +
            pointer_string(latch_address) + " update=" + std::to_string(update)
        );
        count_down_latch_locked(latch_address, latch, update);
        cv_.notify_all();
    }

    bool try_wait_latch(const void* latch_address, std::ptrdiff_t observed_count) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        const auto& latch = latch_state_locked(latch_address, observed_count);
        const bool ready = latch.count == 0;
        append_trace_locked(
            "thread " + std::to_string(id) +
            (ready ? " try_wait latch ready " : " try_wait latch blocked ") +
            pointer_string(latch_address)
        );
        return ready;
    }

    void wait_latch(const void* latch_address, std::ptrdiff_t observed_count) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& latch = latch_state_locked(latch_address, observed_count);
        append_trace_locked(
            "thread " + std::to_string(id) + " wait latch " +
            pointer_string(latch_address)
        );
        if (latch.count == 0) {
            append_trace_locked(
                "thread " + std::to_string(id) + " latch already open " +
                pointer_string(latch_address)
            );
            return;
        }

        if (std::find(latch.waiters.begin(), latch.waiters.end(), id) == latch.waiters.end()) {
            latch.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        append_trace_locked(
            "thread " + std::to_string(id) + " blocked on latch " +
            pointer_string(latch_address)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            deadlocked_ = true;
            append_trace_locked("deadlock: all unfinished threads are blocked");
            cv_.notify_all();
            throw std::runtime_error("deadlock");
        }

        scheduled_thread_ = next;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        append_trace_locked(
            "thread " + std::to_string(id) + " woke on latch " +
            pointer_string(latch_address)
        );
    }

    void arrive_barrier(
        const void* barrier_address,
        std::ptrdiff_t update,
        bool drop,
        std::ptrdiff_t observed_expected,
        std::ptrdiff_t observed_remaining,
        std::size_t observed_phase
    ) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);
        auto& barrier = barrier_state_locked(
            barrier_address,
            observed_expected,
            observed_remaining,
            observed_phase
        );
        append_trace_locked(
            "thread " + std::to_string(id) + " arrive barrier " +
            pointer_string(barrier_address) + " update=" + std::to_string(update)
        );
        if (drop) {
            append_trace_locked(
                "thread " + std::to_string(id) + " drop barrier " +
                pointer_string(barrier_address)
            );
        }
        arrive_barrier_locked(barrier_address, barrier, update, drop);
        cv_.notify_all();
    }

    void wait_barrier(const void* barrier_address, std::size_t phase) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto it = barriers_.find(barrier_address);
        if (it == barriers_.end()) {
            throw std::runtime_error("barrier wait without a managed arrival");
        }
        auto& barrier = it->second;
        append_trace_locked(
            "thread " + std::to_string(id) + " wait barrier " +
            pointer_string(barrier_address) + " phase=" + std::to_string(phase)
        );
        if (barrier.phase > phase) {
            append_trace_locked(
                "thread " + std::to_string(id) + " barrier phase already complete " +
                pointer_string(barrier_address)
            );
            return;
        }
        if (barrier.phase < phase) {
            throw std::runtime_error("barrier wait token is ahead of the managed barrier phase");
        }

        const BarrierWaiter waiter{id, phase};
        const auto duplicate = std::any_of(barrier.waiters.begin(), barrier.waiters.end(), [&](const auto& existing) {
            return existing.thread_id == waiter.thread_id && existing.phase == waiter.phase;
        });
        if (!duplicate) {
            barrier.waiters.push_back(waiter);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        append_trace_locked(
            "thread " + std::to_string(id) + " blocked on barrier " +
            pointer_string(barrier_address)
        );

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            deadlocked_ = true;
            append_trace_locked("deadlock: all unfinished threads are blocked");
            cv_.notify_all();
            throw std::runtime_error("deadlock");
        }

        scheduled_thread_ = next;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        append_trace_locked(
            "thread " + std::to_string(id) + " woke on barrier " +
            pointer_string(barrier_address)
        );
    }

    void park(const void* parker_address) override {
        const auto id = current_thread_id_;
        if (id < 0) return;

        std::unique_lock lock(mutex_);
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");

        auto& parker = parkers_[parker_address];
        append_trace_locked("thread " + std::to_string(id) + " park " + pointer_string(parker_address));
        if (parker.permit) {
            parker.permit = false;
            append_trace_locked("thread " + std::to_string(id) + " consumed park permit " + pointer_string(parker_address));
            return;
        }

        if (std::find(parker.waiters.begin(), parker.waiters.end(), id) == parker.waiters.end()) {
            parker.waiters.push_back(id);
        }
        blocked_[static_cast<std::size_t>(id)] = true;
        append_trace_locked("thread " + std::to_string(id) + " parked " + pointer_string(parker_address));

        const int next = choose_next_active_or_timeout_locked(static_cast<std::size_t>(id));
        if (next < 0) {
            deadlocked_ = true;
            append_trace_locked("deadlock: all unfinished threads are blocked");
            cv_.notify_all();
            throw std::runtime_error("deadlock");
        }

        scheduled_thread_ = next;
        cv_.notify_all();
        cv_.wait(lock, [&] {
            return abort_requested_ ||
                deadlocked_ ||
                (!blocked_[static_cast<std::size_t>(id)] && scheduled_thread_ == id);
        });
        check_abort_locked(id);
        if (deadlocked_) throw std::runtime_error("deadlock");
        append_trace_locked("thread " + std::to_string(id) + " resumed from park " + pointer_string(parker_address));
    }

    void unpark(const void* parker_address) override {
        const auto id = current_thread_id_;
        std::lock_guard lock(mutex_);
        check_abort_locked(id);

        auto& parker = parkers_[parker_address];
        append_trace_locked("thread " + std::to_string(id) + " unpark " + pointer_string(parker_address));
        if (!parker.waiters.empty()) {
            const int waiter = parker.waiters.front();
            parker.waiters.pop_front();
            blocked_[static_cast<std::size_t>(waiter)] = false;
            append_trace_locked("thread " + std::to_string(waiter) + " unparked " + pointer_string(parker_address));
        } else {
            parker.permit = true;
            append_trace_locked("park permit available " + pointer_string(parker_address));
        }
        cv_.notify_all();
    }

    std::string trace_string() const {
        std::lock_guard lock(mutex_);
        std::ostringstream out;
        out << "schedule:";
        for (int choice : reported_schedule_) out << " " << choice;
        out << "\n";
        std::vector<std::string> decision_lines;
        decision_lines.reserve(schedule_decisions_.size());
        for (const auto& decision : schedule_decisions_) {
            std::ostringstream line;
            line << "  #" << decision.switch_position << " ";
            if (decision.thread_id >= 0) {
                line << "thread " << decision.thread_id << " @ ";
            }
            line << (decision.location.empty() ? "unknown" : decision.location)
                << " -> " << decision.chosen_thread
                << " runnable:";
            for (const int thread : decision.runnable_threads) {
                line << " " << thread;
            }
            if (!decision.runnable_operations.empty()) {
                line << " operations:";
                for (const auto& operation : decision.runnable_operations) {
                    line << " " << schedule_operation_token(operation);
                }
            }
            auto text = line.str();
            if (trace_filter_.accepts(text)) {
                decision_lines.push_back(std::move(text));
            }
        }
        if (!decision_lines.empty()) {
            out << "schedule decisions:\n";
            for (const auto& line : decision_lines) {
                out << line << "\n";
            }
        }
        for (const auto& line : trace_) out << line << "\n";
        return out.str();
    }

    std::vector<int> reported_schedule() const {
        std::lock_guard lock(mutex_);
        return reported_schedule_;
    }

    std::vector<ScheduleDecision> schedule_decisions() const {
        std::lock_guard lock(mutex_);
        return schedule_decisions_;
    }

    std::vector<TraceEventRecord> trace_events() const {
        std::lock_guard lock(mutex_);
        return trace_events_;
    }

    std::vector<MemoryEventRecord> memory_events() const {
        std::lock_guard lock(mutex_);
        return memory_events_;
    }

    std::vector<StmEventRecord> stm_events() const {
        std::lock_guard lock(mutex_);
        return stm_events_;
    }

    std::vector<SourceAccessEventRecord> source_accesses() const {
        std::lock_guard lock(mutex_);
        return source_accesses_;
    }

    std::vector<SynchronizationEventRecord> synchronization_events() const {
        std::lock_guard lock(mutex_);
        return synchronization_events_;
    }

    void memory_event(const MemoryEvent& event) override {
        std::lock_guard lock(mutex_);
        const auto operation = active_operation_context_locked(current_thread_id_);
        memory_events_.push_back(detail::make_memory_event_record(
            memory_events_.size(),
            current_thread_id_,
            event,
            event_sequence_++,
            operation ? &*operation : nullptr
        ));
    }

    void stm_event(const stm::Event& event) override {
        {
            std::lock_guard lock(mutex_);
            const auto operation = active_operation_context_locked(current_thread_id_);
            stm_events_.push_back(detail::make_stm_event_record(
                stm_events_.size(),
                current_thread_id_,
                event,
                event_sequence_++,
                operation ? &*operation : nullptr
            ));
        }
        Runtime::stm_event(event);
    }

    void source_access_event(const SourceAccessEvent& event) override {
        std::lock_guard lock(mutex_);
        const auto operation = active_operation_context_locked(current_thread_id_);
        source_accesses_.push_back(detail::make_source_access_event_record(
            source_accesses_.size(),
            current_thread_id_,
            event,
            event_sequence_++,
            operation ? &*operation : nullptr
        ));
    }

    void synchronization_event(const SynchronizationEvent& event) override {
        std::lock_guard lock(mutex_);
        const auto operation = active_operation_context_locked(current_thread_id_);
        synchronization_events_.push_back(detail::make_synchronization_event_record(
            synchronization_events_.size(),
            current_thread_id_,
            event,
            event_sequence_++,
            operation ? &*operation : nullptr
        ));
    }

    std::optional<std::string> replay_schedule_error() const {
        std::lock_guard lock(mutex_);
        if (schedule_exhausted_) {
            return "lincheck replay schedule ended before execution completed";
        }
        if (schedule_non_runnable_choice_) {
            return "lincheck replay schedule selected a thread that was not runnable";
        }
        if (schedule_index_ < schedule_.size()) {
            return "lincheck replay schedule contains unused thread choices";
        }
        return std::nullopt;
    }

    std::vector<std::string> warnings() const {
        std::lock_guard lock(mutex_);
        return warnings_;
    }

    class ThreadScope {
    public:
        ThreadScope(CooperativeScheduler& scheduler, int thread_id)
            : previous_(current_thread_id_) {
            current_thread_id_ = thread_id;
            runtime_ = std::make_unique<ScopedRuntime>(&scheduler);
        }

        ~ThreadScope() {
            runtime_.reset();
            current_thread_id_ = previous_;
        }

    private:
        int previous_;
        std::unique_ptr<ScopedRuntime> runtime_;
    };

private:
    int choose_initial_thread_locked() {
        const auto choices = active_threads_locked();
        if (!schedule_.empty()) {
            const int candidate = schedule_[0];
            schedule_index_ = 1;
            if (std::find(choices.begin(), choices.end(), candidate) != choices.end()) {
                record_schedule_decision_locked(choices, candidate, -1, "initial");
                return candidate;
            }
            schedule_non_runnable_choice_ = true;
            append_trace_locked(
                "schedule choice thread " + std::to_string(candidate) +
                " was not runnable at start"
            );
        }
        const int fallback = choices.empty() ? -1 : choices.front();
        if (fallback >= 0) {
            schedule_exhausted_ = true;
            record_schedule_decision_locked(choices, fallback, -1, "initial");
        }
        return fallback;
    }

    int choose_scheduled_target_locked(int current, const char* location) {
        const auto choices = active_threads_locked();
        if (schedule_index_ < schedule_.size()) {
            const int candidate = schedule_[schedule_index_++];
            if (std::find(choices.begin(), choices.end(), candidate) != choices.end()) {
                record_schedule_decision_locked(choices, candidate, current, location);
                return candidate;
            }
            schedule_non_runnable_choice_ = true;
            append_trace_locked(
                "schedule choice thread " + std::to_string(candidate) +
                " was not runnable; continuing thread " + std::to_string(current)
            );
            record_schedule_decision_locked(choices, current, current, location);
            return current;
        }
        schedule_exhausted_ = true;
        record_schedule_decision_locked(choices, current, current, location);
        return current;
    }

    std::vector<int> active_threads_locked() const {
        std::vector<int> choices;
        choices.reserve(finished_.size());
        for (std::size_t thread = 0; thread < finished_.size(); ++thread) {
            if (is_active_locked(static_cast<int>(thread))) {
                choices.push_back(static_cast<int>(thread));
            }
        }
        return choices;
    }

    void record_schedule_decision_locked(std::vector<int> choices, int chosen, int thread_id, const char* location) {
        auto runnable_operations = active_operation_contexts_locked(choices);
        reported_schedule_.push_back(chosen);
        schedule_decisions_.push_back(ScheduleDecision{
            .switch_position = schedule_decisions_.size(),
            .thread_id = thread_id,
            .location = location == nullptr ? "" : std::string(location),
            .runnable_threads = std::move(choices),
            .chosen_thread = chosen,
            .runnable_operations = std::move(runnable_operations)
        });
    }

    std::vector<OperationContext> active_operation_contexts_locked(const std::vector<int>& choices) const {
        std::vector<OperationContext> result;
        result.reserve(choices.size());
        for (const int thread : choices) {
            if (
                thread >= 0 &&
                static_cast<std::size_t>(thread) < active_operations_.size() &&
                active_operations_[static_cast<std::size_t>(thread)]
            ) {
                result.push_back(*active_operations_[static_cast<std::size_t>(thread)]);
            }
        }
        return result;
    }

    std::optional<OperationContext> active_operation_context_locked(int thread_id) const {
        if (
            thread_id < 0 ||
            static_cast<std::size_t>(thread_id) >= active_operations_.size()
        ) {
            return std::nullopt;
        }
        return active_operations_[static_cast<std::size_t>(thread_id)];
    }

    void record_trace_event_locked(int thread_id, std::string kind, std::string description) {
        const auto operation = active_operation_context_locked(thread_id);
        trace_events_.push_back(detail::make_trace_event_record(
            trace_events_.size(),
            thread_id,
            std::move(kind),
            std::move(description),
            event_sequence_++,
            operation ? &*operation : nullptr
        ));
    }

    static std::string schedule_token_component(std::string value) {
        if (value.empty()) return "_";
        for (char& ch : value) {
            const auto uch = static_cast<unsigned char>(ch);
            if (
                std::isalnum(uch) == 0 &&
                ch != '_' &&
                ch != '-' &&
                ch != '.' &&
                ch != ':' &&
                ch != '/'
            ) {
                ch = '_';
            }
        }
        return value;
    }

    static std::string schedule_operation_token(const OperationContext& operation) {
        std::ostringstream out;
        out << operation.thread_id << "="
            << schedule_token_component(operation.name)
            << "@" << operation.actor_index
            << "#" << operation.operation_index;
        if (
            !operation.group.empty() ||
            !operation.independence_group.empty() ||
            operation.non_parallel ||
            operation.one_shot ||
            operation.exception_results
        ) {
            out << "[";
            bool first = true;
            auto append = [&](const std::string& value) {
                if (!first) out << ";";
                out << value;
                first = false;
            };
            if (!operation.group.empty()) append("group=" + schedule_token_component(operation.group));
            if (!operation.independence_group.empty()) {
                append("independent=" + schedule_token_component(operation.independence_group));
            }
            if (operation.non_parallel) append("non_parallel");
            if (operation.one_shot) append("one_shot");
            if (operation.exception_results) append("exceptions_as_results");
            out << "]";
        }
        return out.str();
    }

    int choose_next_active_locked(std::size_t after) const {
        if (finished_.empty()) return -1;
        for (std::size_t offset = 1; offset <= finished_.size(); ++offset) {
            const auto candidate = static_cast<int>((after + offset) % finished_.size());
            if (is_active_locked(candidate)) return candidate;
        }
        return -1;
    }

    int choose_next_active_or_timeout_locked(std::size_t after) {
        if (abort_requested_) return -1;
        const int next = choose_next_active_locked(after);
        if (next >= 0) return next;
        if (const int condition_waiter = timeout_next_condition_waiter_locked(); condition_waiter >= 0) {
            return condition_waiter;
        }
        if (const int lock_waiter = timeout_next_lock_waiter_locked(); lock_waiter >= 0) {
            return lock_waiter;
        }
        return timeout_next_semaphore_waiter_locked();
    }

    bool is_active_locked(int thread_id) const {
        return thread_id >= 0 &&
            static_cast<std::size_t>(thread_id) < finished_.size() &&
            ready_[static_cast<std::size_t>(thread_id)] &&
            !finished_[static_cast<std::size_t>(thread_id)] &&
            !aborted_[static_cast<std::size_t>(thread_id)] &&
            !blocked_[static_cast<std::size_t>(thread_id)];
    }

    void check_abort_locked(int thread_id) const {
        if (
            abort_requested_ &&
            thread_id >= 0 &&
            static_cast<std::size_t>(thread_id) < aborted_.size() &&
            aborted_[static_cast<std::size_t>(thread_id)] &&
            !finished_[static_cast<std::size_t>(thread_id)]
        ) {
            append_trace_locked("thread " + std::to_string(thread_id) + " aborted");
            throw ScheduleAbortError("schedule aborted");
        }
    }

    void append_trace_locked(std::string line) const {
        if (trace_filter_.accepts(line)) {
            trace_.push_back(std::move(line));
        }
    }

    static std::string pointer_string(const void* address) {
        return stable_object_id(address);
    }

    struct LockState {
        int owner = -1;
        int depth = 0;
        std::deque<int> waiters;
        std::unordered_map<int, int> shared_owners;
        std::deque<int> shared_waiters;
    };

    struct ConditionState {
        std::deque<int> waiters;
    };

    struct AtomicWaitState {
        std::deque<int> waiters;
    };

    struct ParkState {
        bool permit = false;
        std::deque<int> waiters;
    };

    struct SemaphoreState {
        std::ptrdiff_t permits = 0;
        std::deque<int> waiters;
    };

    struct LatchState {
        std::ptrdiff_t count = 0;
        std::deque<int> waiters;
    };

    struct BarrierWaiter {
        int thread_id = -1;
        std::size_t phase = 0;
    };

    struct BarrierState {
        std::ptrdiff_t expected = 0;
        std::ptrdiff_t remaining = 0;
        std::size_t phase = 0;
        std::deque<BarrierWaiter> waiters;
    };

    SemaphoreState& semaphore_state_locked(const void* address, std::ptrdiff_t observed_permits) {
        auto [it, inserted] = semaphores_.try_emplace(address);
        if (inserted) {
            it->second.permits = observed_permits;
        }
        return it->second;
    }

    LatchState& latch_state_locked(const void* address, std::ptrdiff_t observed_count) {
        auto [it, inserted] = latches_.try_emplace(address);
        if (inserted) {
            it->second.count = observed_count;
        }
        return it->second;
    }

    BarrierState& barrier_state_locked(
        const void* address,
        std::ptrdiff_t observed_expected,
        std::ptrdiff_t observed_remaining,
        std::size_t observed_phase
    ) {
        auto [it, inserted] = barriers_.try_emplace(address);
        if (inserted) {
            it->second.expected = observed_expected;
            it->second.remaining = observed_remaining;
            it->second.phase = observed_phase;
        }
        return it->second;
    }

    void require_single_depth_lock_locked(const void* address, int thread_id) const {
        auto it = locks_.find(address);
        if (it == locks_.end() || it->second.owner != thread_id || it->second.depth != 1) {
            throw std::runtime_error("condition_variable wait requires single-depth lock ownership");
        }
    }

    void release_lock_locked(const void* address, int thread_id) {
        auto it = locks_.find(address);
        if (it == locks_.end() || it->second.owner != thread_id || it->second.depth == 0) {
            throw std::runtime_error("unlock by non-owner");
        }

        auto& state = it->second;
        --state.depth;
        append_trace_locked("thread " + std::to_string(thread_id) + " lock release " + pointer_string(address));
        if (state.depth != 0) return;

        if (!state.waiters.empty()) {
            const int next_owner = state.waiters.front();
            state.waiters.pop_front();
            state.owner = next_owner;
            state.depth = 1;
            blocked_[static_cast<std::size_t>(next_owner)] = false;
            lock_timed_out_[static_cast<std::size_t>(next_owner)] = false;
            append_trace_locked(
                "thread " + std::to_string(next_owner) + " unblocked on lock " + pointer_string(address)
            );
        } else {
            state.owner = -1;
            wake_shared_lock_waiters_locked(address, state);
        }
    }

    void release_shared_lock_locked(const void* address, int thread_id) {
        auto it = locks_.find(address);
        if (it == locks_.end()) {
            throw std::runtime_error("shared unlock by non-owner");
        }

        auto& state = it->second;
        auto shared = state.shared_owners.find(thread_id);
        if (shared == state.shared_owners.end() || shared->second == 0) {
            throw std::runtime_error("shared unlock by non-owner");
        }

        --shared->second;
        append_trace_locked("thread " + std::to_string(thread_id) + " shared lock release " + pointer_string(address));
        if (shared->second == 0) {
            state.shared_owners.erase(shared);
        }
        if (!state.shared_owners.empty() || state.owner != -1) return;

        if (!state.waiters.empty()) {
            const int next_owner = state.waiters.front();
            state.waiters.pop_front();
            state.owner = next_owner;
            state.depth = 1;
            blocked_[static_cast<std::size_t>(next_owner)] = false;
            lock_timed_out_[static_cast<std::size_t>(next_owner)] = false;
            append_trace_locked(
                "thread " + std::to_string(next_owner) + " unblocked on lock " + pointer_string(address)
            );
            return;
        }

        wake_shared_lock_waiters_locked(address, state);
    }

    void wake_shared_lock_waiters_locked(const void* address, LockState& state) {
        if (state.owner != -1) return;
        while (!state.shared_waiters.empty()) {
            const int waiter = state.shared_waiters.front();
            state.shared_waiters.pop_front();
            ++state.shared_owners[waiter];
            blocked_[static_cast<std::size_t>(waiter)] = false;
            lock_timed_out_[static_cast<std::size_t>(waiter)] = false;
            append_trace_locked(
                "thread " + std::to_string(waiter) + " unblocked on shared lock " +
                pointer_string(address)
            );
        }
    }

    void notify_one_condition_locked(const void* condition_address) {
        auto& condition = conditions_[condition_address];
        if (condition.waiters.empty()) {
            append_trace_locked("condition_variable " + pointer_string(condition_address) + " notify_one had no waiters");
            return;
        }

        const int waiter = condition.waiters.front();
        condition.waiters.pop_front();
        blocked_[static_cast<std::size_t>(waiter)] = false;
        condition_timed_out_[static_cast<std::size_t>(waiter)] = false;
        append_trace_locked(
            "thread " + std::to_string(waiter) + " notified on condition_variable " +
            pointer_string(condition_address)
        );
    }

    void notify_one_atomic_locked(const void* atomic_address) {
        auto& atomic = atomic_waits_[atomic_address];
        if (atomic.waiters.empty()) {
            append_trace_locked("atomic " + pointer_string(atomic_address) + " notify_one had no waiters");
            return;
        }

        const int waiter = atomic.waiters.front();
        atomic.waiters.pop_front();
        blocked_[static_cast<std::size_t>(waiter)] = false;
        append_trace_locked(
            "thread " + std::to_string(waiter) + " notified on atomic " +
            pointer_string(atomic_address)
        );
    }

    void release_semaphore_locked(
        const void* semaphore_address,
        SemaphoreState& semaphore,
        std::ptrdiff_t update
    ) {
        if (update < 0) {
            throw std::runtime_error("negative semaphore release");
        }
        semaphore.permits += update;
        if (update == 0 && semaphore.waiters.empty()) {
            append_trace_locked("semaphore " + pointer_string(semaphore_address) + " release update was zero");
            return;
        }
        if (semaphore.waiters.empty()) {
            append_trace_locked("semaphore " + pointer_string(semaphore_address) + " release had no waiters");
        }
        while (semaphore.permits > 0 && !semaphore.waiters.empty()) {
            const int waiter = semaphore.waiters.front();
            semaphore.waiters.pop_front();
            --semaphore.permits;
            blocked_[static_cast<std::size_t>(waiter)] = false;
            semaphore_timed_out_[static_cast<std::size_t>(waiter)] = false;
            append_trace_locked(
                "thread " + std::to_string(waiter) + " unblocked on semaphore " +
                pointer_string(semaphore_address)
            );
        }
    }

    void count_down_latch_locked(
        const void* latch_address,
        LatchState& latch,
        std::ptrdiff_t update
    ) {
        if (update < 0 || update > latch.count) {
            throw std::runtime_error("invalid latch count_down");
        }
        latch.count -= update;
        if (latch.count > 0) {
            append_trace_locked(
                "latch " + pointer_string(latch_address) +
                " count=" + std::to_string(latch.count)
            );
            return;
        }

        if (latch.waiters.empty()) {
            append_trace_locked("latch " + pointer_string(latch_address) + " opened with no waiters");
            return;
        }
        while (!latch.waiters.empty()) {
            const int waiter = latch.waiters.front();
            latch.waiters.pop_front();
            blocked_[static_cast<std::size_t>(waiter)] = false;
            append_trace_locked(
                "thread " + std::to_string(waiter) + " unblocked on latch " +
                pointer_string(latch_address)
            );
        }
    }

    void arrive_barrier_locked(
        const void* barrier_address,
        BarrierState& barrier,
        std::ptrdiff_t update,
        bool drop
    ) {
        if (update <= 0 || update > barrier.remaining) {
            throw std::runtime_error("invalid barrier arrive");
        }
        if (drop) {
            if (barrier.expected <= 0) {
                throw std::runtime_error("invalid barrier drop");
            }
            --barrier.expected;
        }

        barrier.remaining -= update;
        if (barrier.remaining > 0) {
            append_trace_locked(
                "barrier " + pointer_string(barrier_address) +
                " phase=" + std::to_string(barrier.phase) +
                " remaining=" + std::to_string(barrier.remaining)
            );
            return;
        }

        const auto completed_phase = barrier.phase;
        ++barrier.phase;
        barrier.remaining = barrier.expected;
        append_trace_locked(
            "barrier " + pointer_string(barrier_address) +
            " phase " + std::to_string(completed_phase) + " complete"
        );

        auto it = barrier.waiters.begin();
        while (it != barrier.waiters.end()) {
            if (it->phase < barrier.phase) {
                const int waiter = it->thread_id;
                blocked_[static_cast<std::size_t>(waiter)] = false;
                append_trace_locked(
                    "thread " + std::to_string(waiter) + " unblocked on barrier " +
                    pointer_string(barrier_address)
                );
                it = barrier.waiters.erase(it);
            } else {
                ++it;
            }
        }
    }

    int timeout_next_condition_waiter_locked() {
        for (auto& [condition_address, condition] : conditions_) {
            for (auto it = condition.waiters.begin(); it != condition.waiters.end(); ++it) {
                const int waiter = *it;
                if (
                    waiter >= 0 &&
                    static_cast<std::size_t>(waiter) < timed_condition_waiting_.size() &&
                    timed_condition_waiting_[static_cast<std::size_t>(waiter)] &&
                    !finished_[static_cast<std::size_t>(waiter)]
                ) {
                    condition.waiters.erase(it);
                    blocked_[static_cast<std::size_t>(waiter)] = false;
                    condition_timed_out_[static_cast<std::size_t>(waiter)] = true;
                    append_trace_locked(
                        "thread " + std::to_string(waiter) + " timed out on condition_variable " +
                        pointer_string(condition_address)
                    );
                    return waiter;
                }
            }
        }
        return -1;
    }

    int timeout_next_lock_waiter_locked() {
        for (auto& [lock_address, lock_state] : locks_) {
            for (auto it = lock_state.waiters.begin(); it != lock_state.waiters.end(); ++it) {
                const int waiter = *it;
                if (
                    waiter >= 0 &&
                    static_cast<std::size_t>(waiter) < timed_lock_waiting_.size() &&
                    timed_lock_waiting_[static_cast<std::size_t>(waiter)] &&
                    !finished_[static_cast<std::size_t>(waiter)]
                ) {
                    lock_state.waiters.erase(it);
                    blocked_[static_cast<std::size_t>(waiter)] = false;
                    lock_timed_out_[static_cast<std::size_t>(waiter)] = true;
                    append_trace_locked(
                        "thread " + std::to_string(waiter) + " timed out on lock " +
                        pointer_string(lock_address)
                    );
                    return waiter;
                }
            }
            for (auto it = lock_state.shared_waiters.begin(); it != lock_state.shared_waiters.end(); ++it) {
                const int waiter = *it;
                if (
                    waiter >= 0 &&
                    static_cast<std::size_t>(waiter) < timed_lock_waiting_.size() &&
                    timed_lock_waiting_[static_cast<std::size_t>(waiter)] &&
                    !finished_[static_cast<std::size_t>(waiter)]
                ) {
                    lock_state.shared_waiters.erase(it);
                    blocked_[static_cast<std::size_t>(waiter)] = false;
                    lock_timed_out_[static_cast<std::size_t>(waiter)] = true;
                    append_trace_locked(
                        "thread " + std::to_string(waiter) + " timed out on shared lock " +
                        pointer_string(lock_address)
                    );
                    return waiter;
                }
            }
        }
        return -1;
    }

    int timeout_next_semaphore_waiter_locked() {
        for (auto& [semaphore_address, semaphore] : semaphores_) {
            for (auto it = semaphore.waiters.begin(); it != semaphore.waiters.end(); ++it) {
                const int waiter = *it;
                if (
                    waiter >= 0 &&
                    static_cast<std::size_t>(waiter) < timed_semaphore_waiting_.size() &&
                    timed_semaphore_waiting_[static_cast<std::size_t>(waiter)] &&
                    !finished_[static_cast<std::size_t>(waiter)]
                ) {
                    semaphore.waiters.erase(it);
                    blocked_[static_cast<std::size_t>(waiter)] = false;
                    semaphore_timed_out_[static_cast<std::size_t>(waiter)] = true;
                    append_trace_locked(
                        "thread " + std::to_string(waiter) + " timed out on semaphore " +
                        pointer_string(semaphore_address)
                    );
                    return waiter;
                }
            }
        }
        return -1;
    }

    void timeout_condition_waiter_locked(const void* condition_address, int thread_id) {
        auto& condition = conditions_[condition_address];
        auto it = std::find(condition.waiters.begin(), condition.waiters.end(), thread_id);
        if (it != condition.waiters.end()) {
            condition.waiters.erase(it);
        }
        blocked_[static_cast<std::size_t>(thread_id)] = false;
        condition_timed_out_[static_cast<std::size_t>(thread_id)] = true;
        scheduled_thread_ = thread_id;
        append_trace_locked(
            "thread " + std::to_string(thread_id) + " timed out on condition_variable " +
            pointer_string(condition_address)
        );
    }

    void timeout_lock_waiter_locked(const void* lock_address, int thread_id) {
        auto& lock_state = locks_[lock_address];
        auto it = std::find(lock_state.waiters.begin(), lock_state.waiters.end(), thread_id);
        bool was_shared_waiter = false;
        if (it != lock_state.waiters.end()) {
            lock_state.waiters.erase(it);
        }
        auto shared_it = std::find(lock_state.shared_waiters.begin(), lock_state.shared_waiters.end(), thread_id);
        if (shared_it != lock_state.shared_waiters.end()) {
            lock_state.shared_waiters.erase(shared_it);
            was_shared_waiter = true;
        }
        blocked_[static_cast<std::size_t>(thread_id)] = false;
        lock_timed_out_[static_cast<std::size_t>(thread_id)] = true;
        scheduled_thread_ = thread_id;
        append_trace_locked(
            "thread " + std::to_string(thread_id) +
            (was_shared_waiter ? " timed out on shared lock " : " timed out on lock ") +
            pointer_string(lock_address)
        );
    }

    void timeout_semaphore_waiter_locked(const void* semaphore_address, int thread_id) {
        auto& semaphore = semaphores_[semaphore_address];
        auto it = std::find(semaphore.waiters.begin(), semaphore.waiters.end(), thread_id);
        if (it != semaphore.waiters.end()) {
            semaphore.waiters.erase(it);
        }
        blocked_[static_cast<std::size_t>(thread_id)] = false;
        semaphore_timed_out_[static_cast<std::size_t>(thread_id)] = true;
        scheduled_thread_ = thread_id;
        append_trace_locked(
            "thread " + std::to_string(thread_id) + " timed out on semaphore " +
            pointer_string(semaphore_address)
        );
    }

    bool has_unfinished_threads_locked() const {
        for (std::size_t i = 0; i < finished_.size(); ++i) {
            if (ready_[i] && !finished_[i]) return true;
        }
        return false;
    }

    void mark_deadlock_if_unfinished_threads_blocked_locked() {
        if (abort_requested_) return;
        if (!has_unfinished_threads_locked()) return;
        deadlocked_ = true;
        append_trace_locked("deadlock: all unfinished threads are blocked");
    }

    inline static thread_local int current_thread_id_ = -1;

    std::vector<int> schedule_;
    std::vector<int> reported_schedule_;
    std::vector<ScheduleDecision> schedule_decisions_;
    std::size_t schedule_index_ = 0;
    std::vector<bool> finished_;
    std::vector<bool> ready_;
    std::vector<bool> blocked_;
    std::vector<bool> aborted_;
    std::vector<bool> timed_condition_waiting_;
    std::vector<bool> condition_timed_out_;
    std::vector<bool> timed_lock_waiting_;
    std::vector<bool> lock_timed_out_;
    std::vector<bool> timed_semaphore_waiting_;
    std::vector<bool> semaphore_timed_out_;
    bool started_ = false;
    bool deadlocked_ = false;
    bool abort_requested_ = false;
    bool schedule_exhausted_ = false;
    bool schedule_non_runnable_choice_ = false;
    int scheduled_thread_ = -1;
    int switch_budget_ = 10000;
    int switch_count_ = 0;
    std::unordered_map<const void*, LockState> locks_;
    std::unordered_map<const void*, ConditionState> conditions_;
    std::unordered_map<const void*, AtomicWaitState> atomic_waits_;
    std::unordered_map<const void*, ParkState> parkers_;
    std::unordered_map<const void*, SemaphoreState> semaphores_;
    std::unordered_map<const void*, LatchState> latches_;
    std::unordered_map<const void*, BarrierState> barriers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    mutable std::vector<std::string> trace_;
    std::vector<std::string> warnings_;
    std::vector<TraceEventRecord> trace_events_;
    std::vector<MemoryEventRecord> memory_events_;
    std::vector<StmEventRecord> stm_events_;
    std::vector<SourceAccessEventRecord> source_accesses_;
    std::vector<SynchronizationEventRecord> synchronization_events_;
    std::size_t event_sequence_ = 0;
    std::vector<std::optional<OperationContext>> active_operations_;
    TraceFilter trace_filter_;
};

class ObstructionFreedomError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ObstructionFreedomRuntime final : public Runtime {
public:
    ObstructionFreedomRuntime(int switch_bound, int thread_id, TraceFilter trace_filter = {})
        : switch_bound_(std::max(1, switch_bound)),
          thread_id_(thread_id),
          trace_filter_(std::move(trace_filter)) {}

    void switch_point(const char* location) override {
        append("switch-point " + std::string(location));
        ++switch_count_;
        if (switch_count_ > switch_bound_) {
            throw ObstructionFreedomError("obstruction freedom switch budget exceeded");
        }
    }

    void event(const std::string& description) override {
        append(description);
    }

    void warning(const std::string& message) override {
        detail::append_unique_warning(warnings_, message);
    }

    int thread_id() const override {
        return thread_id_;
    }

    void operation_begin(const OperationContext& context) override {
        active_operation_ = context;
        active_operation_->thread_id = thread_id_;
        append("operation.start " + active_operation_->name);
    }

    void operation_end(const OperationContext& context) override {
        if (
            active_operation_ &&
            active_operation_->actor_index == context.actor_index &&
            active_operation_->operation_index == context.operation_index
        ) {
            append("operation.finish " + active_operation_->name);
            active_operation_.reset();
        }
    }

    bool manages_locks() const override {
        return true;
    }

    bool manages_parking() const override {
        return true;
    }

    bool manages_atomic_waits() const override {
        return true;
    }

    bool manages_semaphores() const override {
        return true;
    }

    bool manages_latches() const override {
        return true;
    }

    bool manages_barriers() const override {
        return true;
    }

    void lock(const void* address) override {
        auto& state = locks_[address];
        if ((state.owner && state.owner != address) || state.shared_depth > 0) {
            throw ObstructionFreedomError("obstruction freedom blocked on lock");
        }
        state.owner = address;
        ++state.depth;
        append("lock acquired " + pointer_string(address));
    }

    void unlock(const void* address) override {
        auto it = locks_.find(address);
        if (it == locks_.end() || it->second.owner != address || it->second.depth == 0) {
            throw std::runtime_error("unlock by non-owner");
        }
        --it->second.depth;
        append("lock release " + pointer_string(address));
        if (it->second.depth == 0) {
            it->second.owner = nullptr;
        }
    }

    bool try_lock(const void* address) override {
        auto& state = locks_[address];
        if ((state.owner == nullptr && state.shared_depth == 0) || state.owner == address) {
            state.owner = address;
            ++state.depth;
            append("try_lock acquired " + pointer_string(address));
            return true;
        }
        append("try_lock failed " + pointer_string(address));
        return false;
    }

    bool try_lock_for(const void* address, std::chrono::nanoseconds timeout) override {
        auto& state = locks_[address];
        if ((state.owner == nullptr && state.shared_depth == 0) || state.owner == address) {
            state.owner = address;
            ++state.depth;
            append("timed lock acquired " + pointer_string(address));
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            append("timed lock expired " + pointer_string(address));
            return false;
        }

        append("blocked on timed lock " + pointer_string(address));
        throw ObstructionFreedomError("obstruction freedom blocked on timed lock");
    }

    void lock_shared(const void* address) override {
        auto& state = locks_[address];
        if (state.owner != nullptr) {
            throw ObstructionFreedomError("obstruction freedom blocked on shared lock");
        }
        ++state.shared_depth;
        append("shared lock acquired " + pointer_string(address));
    }

    void unlock_shared(const void* address) override {
        auto it = locks_.find(address);
        if (it == locks_.end() || it->second.shared_depth == 0) {
            throw std::runtime_error("shared unlock by non-owner");
        }
        --it->second.shared_depth;
        append("shared lock release " + pointer_string(address));
    }

    bool try_lock_shared(const void* address) override {
        auto& state = locks_[address];
        if (state.owner == nullptr) {
            ++state.shared_depth;
            append("try_lock_shared acquired " + pointer_string(address));
            return true;
        }
        append("try_lock_shared failed " + pointer_string(address));
        return false;
    }

    bool try_lock_shared_for(const void* address, std::chrono::nanoseconds timeout) override {
        auto& state = locks_[address];
        if (state.owner == nullptr) {
            ++state.shared_depth;
            append("timed shared lock acquired " + pointer_string(address));
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            append("timed shared lock expired " + pointer_string(address));
            return false;
        }

        append("blocked on timed shared lock " + pointer_string(address));
        throw ObstructionFreedomError("obstruction freedom blocked on timed shared lock");
    }

    bool owns_lock(const void* address) const override {
        auto it = locks_.find(address);
        return it != locks_.end() && it->second.owner == address && it->second.depth > 0;
    }

    bool owns_shared_lock(const void* address) const override {
        auto it = locks_.find(address);
        return it != locks_.end() && it->second.shared_depth > 0;
    }

    void wait_condition(const void* condition_address, const void* lock_address) override {
        append(
            "blocked on condition_variable " + pointer_string(condition_address) +
            " while holding " + pointer_string(lock_address)
        );
        throw ObstructionFreedomError("obstruction freedom blocked on condition_variable wait");
    }

    std::cv_status wait_condition_for(
        const void* condition_address,
        const void* lock_address,
        std::chrono::nanoseconds
    ) override {
        append(
            "blocked on timed condition_variable " + pointer_string(condition_address) +
            " while holding " + pointer_string(lock_address)
        );
        throw ObstructionFreedomError("obstruction freedom blocked on condition_variable timed wait");
    }

    void notify_one_condition(const void* condition_address) override {
        append("notify_one condition_variable " + pointer_string(condition_address));
    }

    void notify_all_condition(const void* condition_address) override {
        append("notify_all condition_variable " + pointer_string(condition_address));
    }

    void wait_atomic(const void* atomic_address, std::string_view expected) override {
        append(
            "blocked on atomic wait " + pointer_string(atomic_address) +
            " expected=" + std::string(expected)
        );
        throw ObstructionFreedomError("obstruction freedom blocked on atomic wait");
    }

    void notify_one_atomic(const void* atomic_address) override {
        append("notify_one atomic " + pointer_string(atomic_address));
    }

    void notify_all_atomic(const void* atomic_address) override {
        append("notify_all atomic " + pointer_string(atomic_address));
    }

    void acquire_semaphore(const void* semaphore_address, std::ptrdiff_t observed_permits) override {
        auto& state = semaphore_state(semaphore_address, observed_permits);
        if (state.permits > 0) {
            --state.permits;
            append("acquired semaphore " + pointer_string(semaphore_address));
            return;
        }

        append("blocked on semaphore " + pointer_string(semaphore_address));
        throw ObstructionFreedomError("obstruction freedom blocked on semaphore acquire");
    }

    bool try_acquire_semaphore(const void* semaphore_address, std::ptrdiff_t observed_permits) override {
        auto& state = semaphore_state(semaphore_address, observed_permits);
        if (state.permits > 0) {
            --state.permits;
            append("try_acquire semaphore succeeded " + pointer_string(semaphore_address));
            return true;
        }

        append("try_acquire semaphore failed " + pointer_string(semaphore_address));
        return false;
    }

    bool try_acquire_semaphore_for(
        const void* semaphore_address,
        std::ptrdiff_t observed_permits,
        std::chrono::nanoseconds
    ) override {
        auto& state = semaphore_state(semaphore_address, observed_permits);
        if (state.permits > 0) {
            --state.permits;
            append("timed acquire semaphore succeeded " + pointer_string(semaphore_address));
            return true;
        }

        append("timed acquire semaphore expired " + pointer_string(semaphore_address));
        return false;
    }

    void release_semaphore(
        const void* semaphore_address,
        std::ptrdiff_t update,
        std::ptrdiff_t observed_permits
    ) override {
        auto& state = semaphore_state(semaphore_address, observed_permits);
        state.permits += update;
        append(
            "release semaphore " + pointer_string(semaphore_address) +
            " update=" + std::to_string(update)
        );
    }

    void count_down_latch(
        const void* latch_address,
        std::ptrdiff_t update,
        std::ptrdiff_t observed_count
    ) override {
        auto& state = latch_state(latch_address, observed_count);
        if (update < 0 || update > state.count) {
            throw std::runtime_error("invalid latch count_down");
        }
        state.count -= update;
        append(
            "count_down latch " + pointer_string(latch_address) +
            " update=" + std::to_string(update)
        );
    }

    bool try_wait_latch(const void* latch_address, std::ptrdiff_t observed_count) override {
        const auto& state = latch_state(latch_address, observed_count);
        const bool ready = state.count == 0;
        append(
            std::string(ready ? "try_wait latch ready " : "try_wait latch blocked ") +
            pointer_string(latch_address)
        );
        return ready;
    }

    void wait_latch(const void* latch_address, std::ptrdiff_t observed_count) override {
        const auto& state = latch_state(latch_address, observed_count);
        if (state.count == 0) {
            append("latch already open " + pointer_string(latch_address));
            return;
        }

        append("blocked on latch " + pointer_string(latch_address));
        throw ObstructionFreedomError("obstruction freedom blocked on latch wait");
    }

    void arrive_barrier(
        const void* barrier_address,
        std::ptrdiff_t update,
        bool drop,
        std::ptrdiff_t observed_expected,
        std::ptrdiff_t observed_remaining,
        std::size_t observed_phase
    ) override {
        auto& state = barrier_state(
            barrier_address,
            observed_expected,
            observed_remaining,
            observed_phase
        );
        if (update <= 0 || update > state.remaining) {
            throw std::runtime_error("invalid barrier arrive");
        }
        if (drop) {
            if (state.expected <= 0) {
                throw std::runtime_error("invalid barrier drop");
            }
            --state.expected;
        }

        state.remaining -= update;
        append(
            "arrive barrier " + pointer_string(barrier_address) +
            " update=" + std::to_string(update)
        );
        if (drop) {
            append("drop barrier " + pointer_string(barrier_address));
        }
        if (state.remaining == 0) {
            const auto completed_phase = state.phase;
            ++state.phase;
            state.remaining = state.expected;
            append(
                "barrier " + pointer_string(barrier_address) +
                " phase " + std::to_string(completed_phase) + " complete"
            );
        }
    }

    void wait_barrier(const void* barrier_address, std::size_t phase) override {
        auto it = barriers_.find(barrier_address);
        if (it == barriers_.end()) {
            throw std::runtime_error("barrier wait without a managed arrival");
        }
        const auto& state = it->second;
        if (state.phase > phase) {
            append("barrier phase already complete " + pointer_string(barrier_address));
            return;
        }

        append("blocked on barrier " + pointer_string(barrier_address));
        throw ObstructionFreedomError("obstruction freedom blocked on barrier wait");
    }

    void park(const void* parker_address) override {
        append("blocked on park " + pointer_string(parker_address));
        throw ObstructionFreedomError("obstruction freedom blocked on park");
    }

    void unpark(const void* parker_address) override {
        append("unpark " + pointer_string(parker_address));
    }

    std::string trace_string() const {
        std::ostringstream out;
        out << "obstruction-freedom trace:\n";
        for (const auto& line : trace_) out << line << "\n";
        return out.str();
    }

    std::vector<std::string> warnings() const {
        return warnings_;
    }

    std::vector<TraceEventRecord> trace_events() const {
        return trace_events_;
    }

private:
    struct LockState {
        const void* owner = nullptr;
        int depth = 0;
        int shared_depth = 0;
    };

    struct SemaphoreState {
        std::ptrdiff_t permits = 0;
    };

    struct LatchState {
        std::ptrdiff_t count = 0;
    };

    struct BarrierState {
        std::ptrdiff_t expected = 0;
        std::ptrdiff_t remaining = 0;
        std::size_t phase = 0;
    };

    SemaphoreState& semaphore_state(const void* address, std::ptrdiff_t observed_permits) {
        auto [it, inserted] = semaphores_.try_emplace(address);
        if (inserted) {
            it->second.permits = observed_permits;
        }
        return it->second;
    }

    LatchState& latch_state(const void* address, std::ptrdiff_t observed_count) {
        auto [it, inserted] = latches_.try_emplace(address);
        if (inserted) {
            it->second.count = observed_count;
        }
        return it->second;
    }

    BarrierState& barrier_state(
        const void* address,
        std::ptrdiff_t observed_expected,
        std::ptrdiff_t observed_remaining,
        std::size_t observed_phase
    ) {
        auto [it, inserted] = barriers_.try_emplace(address);
        if (inserted) {
            it->second.expected = observed_expected;
            it->second.remaining = observed_remaining;
            it->second.phase = observed_phase;
        }
        return it->second;
    }

    void append(std::string line) {
        trace_events_.push_back(detail::make_trace_event_record(
            trace_events_.size(),
            thread_id_,
            detail::trace_event_kind(line),
            line,
            event_sequence_++,
            active_operation_ ? &*active_operation_ : nullptr
        ));
        if (trace_filter_.accepts(line)) {
            trace_.push_back(std::move(line));
        }
    }

    static std::string pointer_string(const void* address) {
        return stable_object_id(address);
    }

    int switch_bound_;
    int switch_count_ = 0;
    int thread_id_ = 0;
    std::unordered_map<const void*, LockState> locks_;
    std::unordered_map<const void*, SemaphoreState> semaphores_;
    std::unordered_map<const void*, LatchState> latches_;
    std::unordered_map<const void*, BarrierState> barriers_;
    std::vector<std::string> trace_;
    std::vector<std::string> warnings_;
    std::vector<TraceEventRecord> trace_events_;
    std::size_t event_sequence_ = 0;
    std::optional<OperationContext> active_operation_;
    TraceFilter trace_filter_;
};

class ModelCheckingOptions {
public:
    ModelCheckingOptions& iterations(int value) { detail::require_positive_option("iterations", value); config_.iterations = value; return *this; }
    ModelCheckingOptions& invocations_per_iteration(int value) { detail::require_positive_option("invocations_per_iteration", value); config_.invocations_per_iteration = value; return *this; }
    ModelCheckingOptions& threads(int value) { detail::require_positive_option("threads", value); config_.threads = value; return *this; }
    ModelCheckingOptions& actors_per_thread(int value) { detail::require_positive_option("actors_per_thread", value); config_.actors_per_thread = value; return *this; }
    ModelCheckingOptions& actors_before(int value) { detail::require_non_negative_option("actors_before", value); config_.actors_before = value; return *this; }
    ModelCheckingOptions& actors_after(int value) { detail::require_non_negative_option("actors_after", value); config_.actors_after = value; return *this; }
    ModelCheckingOptions& seed(std::uint64_t value) { config_.seed = value; return *this; }
    ModelCheckingOptions& max_schedule_length(int value) { detail::require_positive_option("max_schedule_length", value); config_.max_schedule_length = value; return *this; }
    ModelCheckingOptions& max_switch_points_per_schedule(int value) { detail::require_positive_option("max_switch_points_per_schedule", value); config_.max_switch_points_per_schedule = value; return *this; }
    ModelCheckingOptions& max_context_switches_per_schedule(int value) { detail::require_option_at_least("max_context_switches_per_schedule", value, -1); config_.max_context_switches_per_schedule = value; return *this; }
    ModelCheckingOptions& minimize_failed_scenario(bool value) { config_.minimize_failed_scenario = value; return *this; }
    ModelCheckingOptions& clock_source(ClockSourceKind value) { config_.clock_source = value; return *this; }
    ModelCheckingOptions& memory_model(MemoryModel value) { detail::require_supported_memory_model(value); config_.memory_model = value; return *this; }
    ModelCheckingOptions& operation_context_reduction(bool value = true) { config_.operation_context_reduction = value; return *this; }
    ModelCheckingOptions& event_dependency_reduction(bool value = true) { config_.event_dependency_reduction = value; return *this; }
    ModelCheckingOptions& check_obstruction_freedom(bool value = true) { config_.check_obstruction_freedom = value; return *this; }
    ModelCheckingOptions& obstruction_switch_bound(int value) { detail::require_positive_option("obstruction_switch_bound", value); config_.obstruction_switch_bound = value; return *this; }
    ModelCheckingOptions& trace_filter(TraceFilter value) { config_.trace_filter = std::move(value); return *this; }
    ModelCheckingOptions& trace_include(std::string pattern) { config_.trace_filter.include(std::move(pattern)); return *this; }
    ModelCheckingOptions& trace_exclude(std::string pattern) { config_.trace_filter.exclude(std::move(pattern)); return *this; }

    CheckResult check(std::string name, const TestSpec& spec) const {
        auto result = check(spec);
        detail::annotate_named_check(result, name);
        return result;
    }

    template <typename Builder>
        requires std::is_invocable_v<Builder&&>
    CheckResult check(std::string name, Builder&& builder) const {
        return check(std::move(name), detail::build_test_spec(std::forward<Builder>(builder)));
    }

    CheckResult replay(
        const TestSpec& spec,
        const ExecutionScenario& scenario,
        const std::vector<int>& schedule
    ) const {
        detail::validate_replay_schedule(scenario, schedule);
        detail::validate_scenario_against_spec(spec, scenario);
        LinearizabilityVerifier verifier(spec);
        auto result = finish_schedule_verification(verifier, scenario, run_schedule(spec, scenario, schedule, true));
        result.stats.scenarios_generated = 1;
        result.stats.schedules_generated = 1;
        result.stats.schedules_explored = 1;
        if (!result.success) detail::append_model_checking_stats_section(result);
        return result;
    }

    CheckResult replay(
        const TestSpec& spec,
        const ExecutionScenario& scenario,
        const std::vector<ScheduleDecision>& decisions
    ) const {
        const auto schedule = detail::schedule_from_decisions(scenario, decisions);
        auto result = replay(spec, scenario, schedule);
        validate_replayed_schedule_decisions(decisions, result.schedule_decisions);
        return result;
    }

    CheckResult replay(
        std::string name,
        const TestSpec& spec,
        const ExecutionScenario& scenario,
        const std::vector<int>& schedule
    ) const {
        auto result = replay(spec, scenario, schedule);
        detail::annotate_named_check(result, name);
        return result;
    }

    CheckResult replay(
        std::string name,
        const TestSpec& spec,
        const ExecutionScenario& scenario,
        const std::vector<ScheduleDecision>& decisions
    ) const {
        auto result = replay(spec, scenario, decisions);
        detail::annotate_named_check(result, name);
        return result;
    }

    CheckResult check(std::string name, const TestSpec& spec, const ExecutionScenario& scenario) const {
        auto result = check(spec, scenario);
        detail::annotate_named_check(result, name);
        return result;
    }

    CheckResult check(const TestSpec& spec, const ExecutionScenario& scenario) const {
        if (!scenario.valid()) {
            throw std::invalid_argument("lincheck scenario must contain at least one parallel actor");
        }
        detail::validate_scenario_against_spec(spec, scenario);

        CheckResult success;
        success.scenario = scenario;
        success.stats.scenarios_generated = 1;
        success.warnings = make_clock_source(config_.clock_source)->warnings();
        if (config_.check_obstruction_freedom) {
            auto obstruction_result = check_obstruction_freedom(spec, scenario);
            obstruction_result.stats.scenarios_generated = 1;
            detail::append_unique_warnings(success.warnings, obstruction_result.warnings);
            if (obstruction_result.warnings.empty()) obstruction_result.warnings = success.warnings;
            if (!obstruction_result.success) return obstruction_result;
        }

        auto result = check_scenario(spec, scenario);
        result.stats.scenarios_generated = 1;
        detail::append_unique_warnings(success.warnings, result.warnings);
        if (result.warnings.empty()) result.warnings = success.warnings;
        if (!result.success && config_.minimize_failed_scenario) {
            result = minimize(spec, scenario, result);
            result.stats.scenarios_generated = 1;
            if (result.warnings.empty()) result.warnings = success.warnings;
        }
        if (!result.success) detail::append_model_checking_stats_section(result);
        return result;
    }

    CheckResult check(const TestSpec& spec) const {
        CheckResult success;
        success.warnings = make_clock_source(config_.clock_source)->warnings();
        RandomExecutionGenerator generator(
            spec,
            config_.threads,
            config_.actors_per_thread,
            config_.actors_before,
            config_.actors_after,
            config_.seed
        );

        for (int i = 0; i < config_.iterations; ++i) {
            const auto scenario = generator.next();
            if (!scenario.valid()) continue;
            ++success.stats.scenarios_generated;
            if (config_.check_obstruction_freedom) {
                auto obstruction_result = check_obstruction_freedom(spec, scenario);
                obstruction_result.stats.scenarios_generated = success.stats.scenarios_generated;
                detail::append_unique_warnings(success.warnings, obstruction_result.warnings);
                if (obstruction_result.warnings.empty()) obstruction_result.warnings = success.warnings;
                if (!obstruction_result.success) return obstruction_result;
            }
            auto result = check_scenario(spec, scenario);
            success.stats.schedules_generated += result.stats.schedules_generated;
            success.stats.schedules_explored += result.stats.schedules_explored;
            success.stats.schedules_pruned_by_context_bound += result.stats.schedules_pruned_by_context_bound;
            success.stats.schedules_pruned_by_invocation_budget += result.stats.schedules_pruned_by_invocation_budget;
            success.stats.schedules_pruned_by_operation_context += result.stats.schedules_pruned_by_operation_context;
            success.stats.schedules_pruned_by_event_dependency += result.stats.schedules_pruned_by_event_dependency;
            success.stats.verifications_pruned_by_duplicate_history += result.stats.verifications_pruned_by_duplicate_history;
            success.stats.context_switch_depth_increases += result.stats.context_switch_depth_increases;
            success.stats.max_context_switch_depth_explored =
                std::max(success.stats.max_context_switch_depth_explored, result.stats.max_context_switch_depth_explored);
            result.stats.scenarios_generated = success.stats.scenarios_generated;
            detail::append_unique_warnings(success.warnings, result.warnings);
            if (result.warnings.empty()) result.warnings = success.warnings;
            if (!result.success) {
                if (config_.minimize_failed_scenario) {
                    result = minimize(spec, result.scenario, result);
                    result.stats.scenarios_generated = success.stats.scenarios_generated;
                    if (result.warnings.empty()) result.warnings = success.warnings;
                }
                detail::append_model_checking_stats_section(result);
                return result;
            }
            if (
                success.trace_events.empty() &&
                success.memory_events.empty() &&
                success.stm_events.empty() &&
                success.source_accesses.empty() &&
                success.synchronization_events.empty() &&
                (!result.trace_events.empty() ||
                 !result.memory_events.empty() || !result.stm_events.empty() ||
                 !result.source_accesses.empty() || !result.synchronization_events.empty())
            ) {
                success.scenario = result.scenario;
                success.schedule = result.schedule;
                success.schedule_decisions = result.schedule_decisions;
                success.execution_result = result.execution_result;
                success.trace_events = result.trace_events;
                success.memory_events = result.memory_events;
                success.stm_events = result.stm_events;
                success.source_accesses = result.source_accesses;
                success.synchronization_events = result.synchronization_events;
                success.event_dependencies = result.event_dependencies;
                success.event_dependency_analysis = result.event_dependency_analysis;
                success.operation_dependency_footprints = result.operation_dependency_footprints;
            }
        }
        return success;
    }

private:
    static void validate_replayed_schedule_decisions(
        const std::vector<ScheduleDecision>& expected,
        const std::vector<ScheduleDecision>& observed
    ) {
        if (observed.size() != expected.size()) {
            throw ReplayScheduleError("lincheck replay schedule decisions did not match the replayed switch count");
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (observed[i].switch_position != expected[i].switch_position) {
                throw ReplayScheduleError("lincheck replay schedule decision switch position mismatch");
            }
            if (observed[i].thread_id != expected[i].thread_id || observed[i].location != expected[i].location) {
                throw ReplayScheduleError("lincheck replay schedule decision location mismatch");
            }
            if (observed[i].chosen_thread != expected[i].chosen_thread) {
                throw ReplayScheduleError("lincheck replay schedule decision chosen thread mismatch");
            }
            if (!expected[i].runnable_operations.empty()) {
                if (observed[i].runnable_operations.size() != expected[i].runnable_operations.size()) {
                    throw ReplayScheduleError("lincheck replay schedule decision operation context mismatch");
                }
                for (std::size_t operation = 0; operation < expected[i].runnable_operations.size(); ++operation) {
                    if (!same_operation_context(
                        observed[i].runnable_operations[operation],
                        expected[i].runnable_operations[operation]
                    )) {
                        throw ReplayScheduleError("lincheck replay schedule decision operation context mismatch");
                    }
                }
            }
        }
    }

    static bool same_operation_context(const OperationContext& left, const OperationContext& right) {
        return left.thread_id == right.thread_id &&
            left.actor_index == right.actor_index &&
            left.operation_index == right.operation_index &&
            left.name == right.name &&
            left.group == right.group &&
            left.independence_group == right.independence_group &&
            left.non_parallel == right.non_parallel &&
            left.one_shot == right.one_shot &&
            left.exception_results == right.exception_results;
    }

    CheckResult check_obstruction_freedom(const TestSpec& spec, const ExecutionScenario& scenario) const {
        CheckResult success;
        for (std::size_t thread = 0; thread < scenario.parallel.size(); ++thread) {
            for (std::size_t index = 0; index < scenario.parallel[thread].size(); ++index) {
                const auto& actor = scenario.parallel[thread][index];
                auto object = spec.make_concurrent();
                ObstructionFreedomRuntime runtime(
                    config_.obstruction_switch_bound,
                    static_cast<int>(thread),
                    config_.trace_filter
                );
                std::vector<std::string> runtime_warnings;
                std::mutex runtime_warnings_mutex;
                detail::VectorWarningSink warning_sink(runtime_warnings, runtime_warnings_mutex);

                auto collected_warnings = [&] {
                    std::vector<std::string> warnings;
                    {
                        std::lock_guard lock(runtime_warnings_mutex);
                        warnings = runtime_warnings;
                    }
                    detail::append_unique_warnings(warnings, runtime.warnings());
                    return warnings;
                };

                try {
                    {
                        ScopedWarningSink warning_scope(&warning_sink);
                        detail::run_concurrent_sequence(object.get(), spec, scenario.init);
                    }

                    ScopedRuntime scoped(&runtime);
                    const std::vector<Actor> isolated_actor{actor};
                    auto clock = make_clock_source(config_.clock_source);
                    detail::run_concurrent_sequence(
                        object.get(),
                        spec,
                        isolated_actor,
                        *clock,
                        static_cast<int>(thread),
                        nullptr
                    );
                    detail::append_unique_warnings(success.warnings, collected_warnings());
                } catch (const ObstructionFreedomError& e) {
                    CheckResult result;
                    result.success = false;
                    result.failure = FailureKind::obstruction_freedom;
                    result.exception = std::current_exception();
                    result.message = "obstruction freedom violation";
                    result.scenario = scenario;
                    result.warnings = make_clock_source(config_.clock_source)->warnings();
                    detail::append_unique_warnings(result.warnings, collected_warnings());
                    result.trace_events = runtime.trace_events();
                    detail::refresh_event_dependencies(result);
                    result.trace =
                        detail::format_failure_summary_section(result.failure, result.message) +
                        "operation did not complete in isolation: thread " + std::to_string(thread) +
                        " actor " + std::to_string(index) + " " + actor.to_string() + "\n" +
                        std::string(e.what()) + "\n" +
                        detail::format_warnings_section(result.warnings) +
                        detail::format_trace_events_section(result.trace_events, &config_.trace_filter) +
                        runtime.trace_string();
                    return result;
                } catch (...) {
                    const auto exception = std::current_exception();
                    CheckResult result;
                    result.success = false;
                    result.failure = detail::failure_kind_from_exception(exception);
                    result.exception = exception;
                    result.message = detail::exception_message(exception);
                    result.scenario = scenario;
                    result.warnings = make_clock_source(config_.clock_source)->warnings();
                    detail::append_unique_warnings(result.warnings, collected_warnings());
                    result.trace_events = runtime.trace_events();
                    detail::refresh_event_dependencies(result);
                    result.trace =
                        detail::format_failure_summary_section(result.failure, result.message) +
                        "operation threw while checking obstruction freedom: thread " + std::to_string(thread) +
                        " actor " + std::to_string(index) + " " + actor.to_string() + "\n" +
                        detail::format_warnings_section(result.warnings) +
                        detail::format_trace_events_section(result.trace_events, &config_.trace_filter) +
                        runtime.trace_string();
                    return result;
                }
            }
        }
        return success;
    }

    CheckResult check_scenario(const TestSpec& spec, const ExecutionScenario& scenario) const {
        LinearizabilityVerifier verifier(spec);
        CheckResult last;
        CheckStats stats;
        int explored = 0;
        std::deque<std::vector<int>> frontier;
        std::unordered_set<std::string> seen_prefixes;
        std::unordered_set<std::string> verified_successful_histories;

        const auto initial_choices = schedule_thread_order(
            static_cast<int>(scenario.parallel.size()),
            1,
            {},
            -1,
            0
        );
        for (const int thread : initial_choices) {
            enqueue_schedule_prefix(frontier, seen_prefixes, stats, std::vector<int>{thread});
        }

        int current_context_depth = 0;
        while (!frontier.empty()) {
            auto schedule = take_next_frontier_schedule(frontier, current_context_depth);
            if (!schedule) {
                const int next_depth = next_frontier_context_depth(frontier);
                if (next_depth < 0 || next_depth > context_switch_depth_limit()) break;
                current_context_depth = next_depth;
                ++stats.context_switch_depth_increases;
                continue;
            }

            if (explored >= config_.invocations_per_iteration) {
                ++stats.schedules_pruned_by_invocation_budget;
                break;
            }
            ++explored;
            ++stats.schedules_explored;
            stats.max_context_switch_depth_explored = std::max(
                stats.max_context_switch_depth_explored,
                schedule_context_switches(*schedule)
            );
            auto result = run_schedule(spec, scenario, *schedule);
            const auto history_key = result.success
                ? detail::public_history_equivalence_key(scenario, result.execution_result)
                : std::optional<std::string>{};
            if (history_key && verified_successful_histories.find(*history_key) != verified_successful_histories.end()) {
                ++stats.verifications_pruned_by_duplicate_history;
            } else {
                result = finish_schedule_verification(verifier, scenario, std::move(result));
                if (result.success && history_key) {
                    verified_successful_histories.insert(*history_key);
                }
            }
            last = result;
            last.stats = stats;
            if (!result.success) return last;
            enqueue_observed_alternatives(frontier, seen_prefixes, stats, result);
        }
        last.stats = stats;
        return last;
    }

    CheckResult finish_schedule_verification(
        LinearizabilityVerifier& verifier,
        const ExecutionScenario& scenario,
        CheckResult result
    ) const {
        if (!result.success) return result;
        auto verification = verifier.verify_with_report(scenario, result.execution_result);
        if (!verification.success) {
            result.success = false;
            result.failure = FailureKind::invalid_results;
            result.message = "invalid execution results";
            result.verifier_explanation = std::move(verification.explanation);
            result.trace = result.trace + "\n" +
                detail::format_failure_summary_section(result.failure, result.message) +
                detail::format_model_check_failure_sections(
                    scenario,
                    result.execution_result,
                    result.warnings,
                    result.state_representation,
                    result.verifier_explanation,
                    &result.trace_events,
                    &result.memory_events,
                    &config_.trace_filter,
                    &result.stm_events,
                    &result.source_accesses,
                    &result.synchronization_events,
                    &result.event_dependencies,
                    &result.event_dependency_analysis
                );
        }
        return result;
    }

    CheckResult run_schedule(
        const TestSpec& spec,
        const ExecutionScenario& scenario,
        const std::vector<int>& schedule,
        bool exact_replay = false
    ) const {
        CheckResult result;
        result.scenario = scenario;
        result.schedule = schedule;
        auto clock = make_clock_source(config_.clock_source);
        result.warnings = clock->warnings();
        auto object = spec.make_concurrent();
        std::vector<std::string> runtime_warnings;
        std::mutex runtime_warnings_mutex;
        detail::VectorWarningSink warning_sink(runtime_warnings, runtime_warnings_mutex);

        auto merge_runtime_warnings = [&] {
            std::vector<std::string> warnings;
            {
                std::lock_guard lock(runtime_warnings_mutex);
                warnings = runtime_warnings;
            }
            detail::append_unique_warnings(result.warnings, warnings);
        };

        try {
            {
                ScopedWarningSink warning_scope(&warning_sink);
                result.execution_result.init_results = detail::run_concurrent_sequence(
                    object.get(),
                    spec,
                    scenario.init,
                    *clock,
                    -1,
                    &result.execution_result.init_intervals
                );
            }
            result.execution_result.parallel_results.resize(scenario.parallel.size());
            result.execution_result.parallel_intervals.resize(scenario.parallel.size());

            CooperativeScheduler scheduler(
                schedule,
                scenario.parallel.size(),
                config_.max_switch_points_per_schedule,
                config_.trace_filter
            );
            std::vector<std::thread> workers;
            std::exception_ptr exception;
            std::mutex exception_mutex;

            for (std::size_t t = 0; t < scenario.parallel.size(); ++t) {
                workers.emplace_back([&, t] {
                    CooperativeScheduler::ThreadScope scope(scheduler, static_cast<int>(t));
                    try {
                        scheduler.worker_ready(t);
                        result.execution_result.parallel_results[t] =
                            detail::run_concurrent_sequence(
                                object.get(),
                                spec,
                                scenario.parallel[t],
                                *clock,
                                static_cast<int>(t),
                                &result.execution_result.parallel_intervals[t]
                            );
                    } catch (const ScheduleAbortError&) {
                        // Another worker already found the primary failure.
                    } catch (...) {
                        {
                            std::lock_guard lock(exception_mutex);
                            if (!exception) exception = std::current_exception();
                        }
                        scheduler.request_abort(t);
                    }
                    scheduler.finish_thread(t);
                });
            }
            scheduler.wait_until_ready();
            scheduler.start();
            for (auto& worker : workers) worker.join();
            detail::append_unique_warnings(result.warnings, scheduler.warnings());
            merge_runtime_warnings();
            result.schedule = scheduler.reported_schedule();
            result.schedule_decisions = scheduler.schedule_decisions();
            result.trace_events = scheduler.trace_events();
            result.memory_events = scheduler.memory_events();
            result.stm_events = scheduler.stm_events();
            result.source_accesses = scheduler.source_accesses();
            result.synchronization_events = scheduler.synchronization_events();
            detail::refresh_event_dependencies(result);
            result.trace = scheduler.trace_string();
            if (exact_replay) {
                if (auto schedule_error = scheduler.replay_schedule_error()) {
                    throw ReplayScheduleError(*schedule_error);
                }
            }
            if (exception) {
                result.state_representation = detail::capture_state_representation(spec, object.get());
                result.success = false;
                result.failure = detail::failure_kind_from_exception(exception);
                result.exception = exception;
                result.message = detail::exception_message(exception);
                result.trace +=
                    detail::format_failure_summary_section(result.failure, result.message) +
                    detail::format_model_check_failure_sections(
                        scenario,
                        result.execution_result,
                        result.warnings,
                        result.state_representation,
                        {},
                        &result.trace_events,
                        &result.memory_events,
                        &config_.trace_filter,
                        &result.stm_events,
                        &result.source_accesses,
                        &result.synchronization_events,
                        &result.event_dependencies,
                        &result.event_dependency_analysis
                    );
                return result;
            }
            {
                ScopedWarningSink warning_scope(&warning_sink);
                result.execution_result.post_results = detail::run_concurrent_sequence(
                    object.get(),
                    spec,
                    scenario.post,
                    *clock,
                    -1,
                    &result.execution_result.post_intervals
                );
            }
            merge_runtime_warnings();
            if (auto validation_error = detail::validate_concurrent_object(spec, object.get())) {
                result.state_representation = detail::capture_state_representation(spec, object.get());
                result.success = false;
                result.failure = FailureKind::validation_failure;
                result.message = validation_error->empty() ? "validation failed" : *validation_error;
                result.trace += "\n" +
                    detail::format_failure_summary_section(result.failure, result.message) +
                    "validation failed: " + result.message + "\n" +
                    detail::format_model_check_failure_sections(
                        scenario,
                        result.execution_result,
                        result.warnings,
                        result.state_representation,
                        {},
                        &result.trace_events,
                        &result.memory_events,
                        &config_.trace_filter,
                        &result.stm_events,
                        &result.source_accesses,
                        &result.synchronization_events,
                        &result.event_dependencies,
                        &result.event_dependency_analysis
                    );
                return result;
            }
            result.state_representation = detail::capture_state_representation(spec, object.get());
            return result;
        } catch (const ReplayScheduleError&) {
            throw;
        } catch (...) {
            const auto exception = std::current_exception();
            merge_runtime_warnings();
            result.state_representation = detail::capture_state_representation(spec, object.get());
            result.success = false;
            result.failure = detail::failure_kind_from_exception(exception);
            result.exception = exception;
            result.message = detail::exception_message(exception);
            detail::refresh_event_dependencies(result);
            result.trace +=
                detail::format_failure_summary_section(result.failure, result.message) +
                detail::format_model_check_failure_sections(
                    scenario,
                    result.execution_result,
                    result.warnings,
                    result.state_representation,
                    {},
                    &result.trace_events,
                    &result.memory_events,
                    &config_.trace_filter,
                    &result.stm_events,
                    &result.source_accesses,
                    &result.synchronization_events,
                    &result.event_dependencies,
                    &result.event_dependency_analysis
                );
            return result;
        }
    }

    std::vector<int> schedule_thread_order(
        int threads,
        int length,
        const std::vector<int>& current,
        int previous_thread,
        int context_switches
    ) const {
        std::vector<int> order;
        order.reserve(static_cast<std::size_t>(std::max(0, threads)));
        for (int thread = 0; thread < threads; ++thread) {
            order.push_back(thread);
        }
        return schedule_choice_order(std::move(order), length, current, previous_thread, context_switches);
    }

    std::vector<int> schedule_choice_order(
        std::vector<int> order,
        int length,
        const std::vector<int>& current,
        int previous_thread,
        int context_switches
    ) const {
        if (config_.seed == 0 || order.size() <= 1) return order;
        std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
            return schedule_choice_weight(length, current, previous_thread, context_switches, left) <
                schedule_choice_weight(length, current, previous_thread, context_switches, right);
        });
        return order;
    }

    std::uint64_t schedule_choice_weight(
        int length,
        const std::vector<int>& current,
        int previous_thread,
        int context_switches,
        int choice
    ) const {
        std::uint64_t state = config_.seed ^ 0x243f6a8885a308d3ULL;
        mix_weight(state, static_cast<std::uint64_t>(length));
        mix_weight(state, static_cast<std::uint64_t>(current.size()));
        mix_weight(state, static_cast<std::uint64_t>(previous_thread + 1));
        mix_weight(state, static_cast<std::uint64_t>(context_switches));
        for (const int current_choice : current) {
            mix_weight(state, static_cast<std::uint64_t>(current_choice + 1));
        }
        mix_weight(state, static_cast<std::uint64_t>(choice + 1));
        return state;
    }

    void enqueue_observed_alternatives(
        std::deque<std::vector<int>>& frontier,
        std::unordered_set<std::string>& seen_prefixes,
        CheckStats& stats,
        const CheckResult& result
    ) const {
        const std::size_t decision_limit = std::min(
            {
                result.schedule.size(),
                result.schedule_decisions.size(),
                static_cast<std::size_t>(std::max(0, config_.max_schedule_length))
            }
        );
        for (std::size_t decision_index = 0; decision_index < decision_limit; ++decision_index) {
            std::vector<int> base(result.schedule.begin(), result.schedule.begin() + static_cast<std::ptrdiff_t>(decision_index));
            const int previous_thread = base.empty() ? -1 : base.back();
            const int context_switches = schedule_context_switches(base);
            auto choices = result.schedule_decisions[decision_index].runnable_threads;
            choices = schedule_choice_order(
                std::move(choices),
                static_cast<int>(decision_index + 1),
                base,
                previous_thread,
                context_switches
            );
            for (const int choice : choices) {
                if (choice == result.schedule_decisions[decision_index].chosen_thread) continue;
                if (
                    operation_context_reduction_prunes(
                        result.schedule_decisions[decision_index],
                        result.schedule_decisions[decision_index].chosen_thread,
                        choice
                    )
                ) {
                    ++stats.schedules_pruned_by_operation_context;
                    continue;
                }
                if (
                    event_dependency_reduction_prunes(
                        result,
                        result.schedule_decisions[decision_index],
                        result.schedule_decisions[decision_index].chosen_thread,
                        choice
                    )
                ) {
                    ++stats.schedules_pruned_by_event_dependency;
                    continue;
                }
                auto candidate = base;
                candidate.push_back(choice);
                enqueue_schedule_prefix(frontier, seen_prefixes, stats, std::move(candidate));
            }
        }
    }

    bool operation_context_reduction_prunes(
        const ScheduleDecision& decision,
        int chosen_thread,
        int alternative_thread
    ) const {
        if (!config_.operation_context_reduction || chosen_thread == alternative_thread) {
            return false;
        }
        const auto* chosen = operation_context_for_thread(decision, chosen_thread);
        const auto* alternative = operation_context_for_thread(decision, alternative_thread);
        return chosen != nullptr &&
            alternative != nullptr &&
            !chosen->independence_group.empty() &&
            chosen->independence_group == alternative->independence_group;
    }

    bool event_dependency_reduction_prunes(
        const CheckResult& result,
        const ScheduleDecision& decision,
        int chosen_thread,
        int alternative_thread
    ) const {
        if (!config_.event_dependency_reduction || chosen_thread == alternative_thread) {
            return false;
        }
        if (!result.event_dependency_analysis.consistent) {
            return false;
        }
        const auto* chosen = operation_context_for_thread(decision, chosen_thread);
        const auto* alternative = operation_context_for_thread(decision, alternative_thread);
        if (chosen == nullptr || alternative == nullptr) {
            return false;
        }
        const auto* chosen_footprint = operation_dependency_footprint_for_context(
            result.operation_dependency_footprints,
            *chosen
        );
        const auto* alternative_footprint = operation_dependency_footprint_for_context(
            result.operation_dependency_footprints,
            *alternative
        );
        return chosen_footprint != nullptr &&
            alternative_footprint != nullptr &&
            operation_dependency_footprints_disjoint(*chosen_footprint, *alternative_footprint);
    }

    static const OperationDependencyFootprint* operation_dependency_footprint_for_context(
        const std::vector<OperationDependencyFootprint>& footprints,
        const OperationContext& context
    ) {
        const auto it = std::find_if(
            footprints.begin(),
            footprints.end(),
            [&](const OperationDependencyFootprint& footprint) {
                return same_operation_context_identity(footprint.operation, context);
            }
        );
        return it == footprints.end() ? nullptr : &*it;
    }

    static bool operation_dependency_footprints_disjoint(
        const OperationDependencyFootprint& left,
        const OperationDependencyFootprint& right
    ) {
        if (
            left.event_count == 0 ||
            right.event_count == 0 ||
            !operation_dependency_footprint_has_shared_resource(left) ||
            !operation_dependency_footprint_has_shared_resource(right)
        ) {
            return false;
        }
        for (const auto& left_resource : left.resources) {
            if (!operation_dependency_resource_is_shared(left_resource)) continue;
            for (const auto& right_resource : right.resources) {
                if (!operation_dependency_resource_is_shared(right_resource)) continue;
                if (left_resource == right_resource) {
                    return false;
                }
            }
        }
        return true;
    }

    static bool operation_dependency_footprint_has_shared_resource(
        const OperationDependencyFootprint& footprint
    ) {
        return std::any_of(
            footprint.resources.begin(),
            footprint.resources.end(),
            operation_dependency_resource_is_shared
        );
    }

    static bool operation_dependency_resource_is_shared(const std::string& resource) {
        return !resource.empty() && resource.rfind("tx#", 0) != 0;
    }

    static const OperationContext* operation_context_for_thread(const ScheduleDecision& decision, int thread_id) {
        const auto it = std::find_if(
            decision.runnable_operations.begin(),
            decision.runnable_operations.end(),
            [&](const OperationContext& context) {
                return context.thread_id == thread_id;
            }
        );
        return it == decision.runnable_operations.end() ? nullptr : &*it;
    }

    std::optional<std::vector<int>> take_next_frontier_schedule(
        std::deque<std::vector<int>>& frontier,
        int allowed_context_switches
    ) const {
        auto selected = frontier.end();
        std::uint64_t selected_weight = 0;
        for (auto it = frontier.begin(); it != frontier.end(); ++it) {
            if (schedule_context_switches(*it) > allowed_context_switches) continue;
            if (config_.seed == 0) {
                selected = it;
                break;
            }
            const auto weight = frontier_prefix_weight(*it);
            if (selected == frontier.end() || weight < selected_weight) {
                selected = it;
                selected_weight = weight;
            }
        }
        if (selected == frontier.end()) return std::nullopt;
        auto schedule = std::move(*selected);
        frontier.erase(selected);
        return schedule;
    }

    std::uint64_t frontier_prefix_weight(const std::vector<int>& prefix) const {
        std::uint64_t state = config_.seed ^ 0x6a09e667f3bcc909ULL;
        mix_weight(state, static_cast<std::uint64_t>(prefix.size()));
        mix_weight(state, static_cast<std::uint64_t>(schedule_context_switches(prefix)));
        for (const int choice : prefix) {
            mix_weight(state, static_cast<std::uint64_t>(choice + 1));
        }
        return state;
    }

    static void mix_weight(std::uint64_t& state, std::uint64_t value) {
        state = splitmix64(state ^ (value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2)));
    }

    static std::uint64_t splitmix64(std::uint64_t value) {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    static int next_frontier_context_depth(const std::deque<std::vector<int>>& frontier) {
        int depth = -1;
        for (const auto& prefix : frontier) {
            const int candidate = schedule_context_switches(prefix);
            if (depth < 0 || candidate < depth) {
                depth = candidate;
            }
        }
        return depth;
    }

    void enqueue_schedule_prefix(
        std::deque<std::vector<int>>& frontier,
        std::unordered_set<std::string>& seen_prefixes,
        CheckStats& stats,
        std::vector<int> prefix
    ) const {
        if (prefix.empty()) return;
        if (static_cast<int>(prefix.size()) > config_.max_schedule_length) return;
        if (context_switch_bound_exceeded(schedule_context_switches(prefix))) {
            ++stats.schedules_pruned_by_context_bound;
            return;
        }
        const auto key = schedule_prefix_key(prefix);
        if (seen_prefixes.insert(key).second) {
            ++stats.schedules_generated;
            frontier.push_back(std::move(prefix));
        }
    }

    static int schedule_context_switches(const std::vector<int>& schedule) {
        int switches = 0;
        for (std::size_t i = 1; i < schedule.size(); ++i) {
            if (schedule[i] != schedule[i - 1]) ++switches;
        }
        return switches;
    }

    static std::string schedule_prefix_key(const std::vector<int>& schedule) {
        std::ostringstream out;
        for (const int thread : schedule) {
            out << thread << ',';
        }
        return out.str();
    }

    int context_switch_depth_limit() const {
        if (config_.max_context_switches_per_schedule >= 0) {
            return config_.max_context_switches_per_schedule;
        }
        return std::max(0, config_.max_schedule_length - 1);
    }

    bool context_switch_bound_exceeded(int context_switches) const {
        return config_.max_context_switches_per_schedule >= 0 &&
            context_switches > config_.max_context_switches_per_schedule;
    }

    CheckResult minimize(const TestSpec& spec, const ExecutionScenario&, const CheckResult& original) const {
        CheckResult best = original;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 0; i < best.scenario.init.size() && !changed; ++i) {
                auto candidate = best.scenario;
                candidate.init.erase(candidate.init.begin() + static_cast<std::ptrdiff_t>(i));
                auto result = check_scenario(spec, candidate);
                if (!result.success) {
                    best = result;
                    changed = true;
                    break;
                }
            }
            for (std::size_t i = 0; i < best.scenario.post.size() && !changed; ++i) {
                auto candidate = best.scenario;
                candidate.post.erase(candidate.post.begin() + static_cast<std::ptrdiff_t>(i));
                auto result = check_scenario(spec, candidate);
                if (!result.success) {
                    best = result;
                    changed = true;
                    break;
                }
            }
            for (std::size_t t = 0; t < best.scenario.parallel.size() && !changed; ++t) {
                for (std::size_t i = 0; i < best.scenario.parallel[t].size(); ++i) {
                    auto candidate = best.scenario;
                    candidate.parallel[t].erase(candidate.parallel[t].begin() + static_cast<std::ptrdiff_t>(i));
                    candidate.parallel.erase(
                        std::remove_if(candidate.parallel.begin(), candidate.parallel.end(), [](const auto& actors) {
                            return actors.empty();
                        }),
                        candidate.parallel.end()
                    );
                    if (!detail::still_valid_scenario(candidate)) continue;
                    auto result = check_scenario(spec, candidate);
                    if (!result.success) {
                        best = result;
                        changed = true;
                        break;
                    }
                }
            }
        }
        return best;
    }

    OptionsConfig config_;
};

} // namespace lincheck

#ifndef LC_LOCATION
#define LC_LOCATION ::lincheck::source_location(__FILE__, __LINE__, __func__)
#endif

#ifndef LC_READ
#define LC_READ(value) ::lincheck::read((value), LC_LOCATION)
#endif

#ifndef LC_WRITE
#define LC_WRITE(target, value) ::lincheck::write((target), (value), LC_LOCATION)
#endif

#ifndef LC_CALL
#define LC_CALL(name, fn) ::lincheck::call((name), LC_LOCATION, (fn))
#endif

#ifndef LC_SWITCH
#define LC_SWITCH() ::lincheck::switch_point(LC_LOCATION)
#endif

#ifndef LC_YIELD
#define LC_YIELD() ::lincheck::yield(LC_LOCATION)
#endif

#ifndef LC_THREAD_FENCE
#define LC_THREAD_FENCE(order) ::lincheck::atomic_thread_fence(LC_LOCATION, (order))
#endif

#ifndef LC_SIGNAL_FENCE
#define LC_SIGNAL_FENCE(order) ::lincheck::atomic_signal_fence(LC_LOCATION, (order))
#endif
