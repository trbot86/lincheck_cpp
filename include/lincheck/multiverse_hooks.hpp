#pragma once

#include <lincheck/lincheck.hpp>

#define MULTIVERSE_LINCHECK_TX_BEGIN(read_only, start_clock_or_version) \
    ::lincheck::stm::tx_begin((read_only), (start_clock_or_version))

#define MULTIVERSE_LINCHECK_TX_READ(address, lock_slot, version) \
    ::lincheck::stm::tx_read((address), (lock_slot), (version))

#define MULTIVERSE_LINCHECK_TX_WRITE(address, lock_slot) \
    ::lincheck::stm::tx_write((address), (lock_slot))

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
