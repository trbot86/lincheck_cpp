#pragma once

#include <lincheck/lincheck.hpp>

namespace lincheck::multiverse_hooks_detail {

inline stm::BackendLocationRegistry& tx_field_registry() {
    static stm::BackendLocationRegistry registry("multiverse.tx_field");
    return registry;
}

inline stm::LocationHandle tx_field_location(
    const void* field,
    const void* address,
    std::string label = "tx_field",
    std::string type_name = "tx_field"
) {
    const void* allocation_token = field != nullptr ? field : address;
    return tx_field_registry().get_or_create_self_location(
        allocation_token,
        address,
        std::move(label),
        std::move(type_name)
    );
}

inline void tx_field_location_register(
    const void* field,
    const void* address,
    std::string label,
    std::string type_name
) {
    auto location = tx_field_location(field, address, label, type_name);
    stm::tx_location_register(location, std::move(label), std::move(type_name));
}

template <typename T>
inline void tx_field_location_init(const void* field, const void* address, const T& value) {
    stm::tx_location_init(tx_field_location(field, address), value);
}

inline void tx_field_location_destroy(const void* field, const void* address) {
    const void* allocation_token = field != nullptr ? field : address;
    auto location = tx_field_registry().destroy_location(allocation_token, address);
    if (location) {
        stm::tx_location_destroy(*location);
    }
}

template <typename T>
inline void tx_field_read_value(
    const void* field,
    const void* address,
    const T& value,
    std::uint64_t lock_slot,
    std::uint64_t version
) {
    stm::tx_read_value(tx_field_location(field, address), value, lock_slot, version);
}

template <typename T>
inline void tx_field_write_value(
    const void* field,
    const void* address,
    const T& value,
    std::uint64_t lock_slot
) {
    stm::tx_write_value(tx_field_location(field, address), value, lock_slot);
}

} // namespace lincheck::multiverse_hooks_detail

#define MULTIVERSE_LINCHECK_TX_BEGIN(read_only, start_clock_or_version) \
    ::lincheck::stm::tx_begin((read_only), (start_clock_or_version))

#define MULTIVERSE_LINCHECK_TX_LOCATION_INIT(address, value) \
    ::lincheck::stm::tx_location_init((address), (value))

#define MULTIVERSE_LINCHECK_TX_LOCATION_REGISTER(address, label, type_name) \
    ::lincheck::stm::tx_location_register((address), (label), (type_name))

#define MULTIVERSE_LINCHECK_TX_LOCATION_DESTROY(address) \
    ::lincheck::stm::tx_location_destroy((address))

#define MULTIVERSE_LINCHECK_TX_READ(address, lock_slot, version) \
    ::lincheck::stm::tx_read((address), (lock_slot), (version))

#define MULTIVERSE_LINCHECK_TX_WRITE(address, lock_slot) \
    ::lincheck::stm::tx_write((address), (lock_slot))

#define MULTIVERSE_LINCHECK_TX_READ_VALUE(address, value, lock_slot, version) \
    ::lincheck::stm::tx_read_value((address), (value), (lock_slot), (version))

#define MULTIVERSE_LINCHECK_TX_WRITE_VALUE(address, value, lock_slot) \
    ::lincheck::stm::tx_write_value((address), (value), (lock_slot))

#define MULTIVERSE_LINCHECK_TX_FIELD_LOCATION_REGISTER(field, address, label, type_name) \
    ::lincheck::multiverse_hooks_detail::tx_field_location_register((field), (address), (label), (type_name))

#define MULTIVERSE_LINCHECK_TX_FIELD_LOCATION_INIT(field, address, value) \
    ::lincheck::multiverse_hooks_detail::tx_field_location_init((field), (address), (value))

#define MULTIVERSE_LINCHECK_TX_FIELD_LOCATION_DESTROY(field, address) \
    ::lincheck::multiverse_hooks_detail::tx_field_location_destroy((field), (address))

#define MULTIVERSE_LINCHECK_TX_FIELD_READ_VALUE(field, address, value, lock_slot, version) \
    ::lincheck::multiverse_hooks_detail::tx_field_read_value((field), (address), (value), (lock_slot), (version))

#define MULTIVERSE_LINCHECK_TX_FIELD_WRITE_VALUE(field, address, value, lock_slot) \
    ::lincheck::multiverse_hooks_detail::tx_field_write_value((field), (address), (value), (lock_slot))

#define MULTIVERSE_LINCHECK_TX_VALIDATE_BEGIN() \
    ::lincheck::stm::tx_validate_begin()

#define MULTIVERSE_LINCHECK_TX_VALIDATE_END(success) \
    ::lincheck::stm::tx_validate_end((success))

#define MULTIVERSE_LINCHECK_TX_LOCK_ATTEMPT(lock_slot) \
    ::lincheck::stm::tx_lock_attempt((lock_slot))

#define MULTIVERSE_LINCHECK_TX_LOCK_ACQUIRED(lock_slot) \
    ::lincheck::stm::tx_lock_acquired((lock_slot))

#define MULTIVERSE_LINCHECK_TX_LOCK_FAILED(lock_slot) \
    ::lincheck::stm::tx_lock_failed((lock_slot))

#define MULTIVERSE_LINCHECK_TX_LOCK_RELEASED(lock_slot) \
    ::lincheck::stm::tx_lock_released((lock_slot))

#define MULTIVERSE_LINCHECK_TX_COMMIT_ATTEMPT() \
    ::lincheck::stm::tx_commit_attempt()

#define MULTIVERSE_LINCHECK_TX_COMMIT_SUCCESS(commit_clock) \
    ::lincheck::stm::tx_commit_success((commit_clock))

#define MULTIVERSE_LINCHECK_TX_ABORT(reason) \
    ::lincheck::stm::tx_abort((reason))

#define MULTIVERSE_LINCHECK_TX_RETRY(reason, attempt) \
    ::lincheck::stm::tx_retry((reason), (attempt))

#define MULTIVERSE_LINCHECK_TX_ATTEMPT_METADATA(logical_transaction_id, attempt) \
    ::lincheck::stm::tx_attempt_metadata((logical_transaction_id), (attempt))
