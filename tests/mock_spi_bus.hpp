#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "mstar/flash/spi_bus.hpp"

namespace mstar::test {

struct RecordedSpiTransaction {
    std::vector<uint8_t> tx;
    std::vector<uint8_t> rx; // requested rx contents for this call
};

/// Records every transaction and plays back canned responses for rx, so
/// SpiNand's command sequences can be tested without hardware.
///
/// Queued responses are consumed one per call that requests a non-empty
/// rx, except the last queued response, which sticks and is replayed
/// forever once reached — this lets a test express "busy, then busy,
/// then ready forever after" without having to queue hundreds of
/// identical entries to cover a poll loop's worst case.
class MockSpiBus final : public SpiBus {
public:
    Result<void> transaction(std::span<const uint8_t> tx, std::span<uint8_t> rx) override {
        if (failNextTransaction) {
            failNextTransaction = false;
            transactions.push_back(
                RecordedSpiTransaction{std::vector<uint8_t>(tx.begin(), tx.end()), {}});
            return std::unexpected(
                Error{ErrorCode::FlashProtocol, "mock SPI transaction failure"});
        }
        if (!rx.empty()) {
            fillFromQueue(rx);
        }
        transactions.push_back(RecordedSpiTransaction{
            std::vector<uint8_t>(tx.begin(), tx.end()), std::vector<uint8_t>(rx.begin(), rx.end())});
        return {};
    }

    /// Queues bytes to be returned by the next call(s) that request rx.
    void queueResponse(std::vector<uint8_t> bytes) {
        responseQueue.push_back(std::move(bytes));
    }

    std::vector<RecordedSpiTransaction> transactions;
    bool failNextTransaction = false;

private:
    void fillFromQueue(std::span<uint8_t> out) {
        if (responseQueue.empty()) {
            return;
        }
        auto& front = responseQueue.front();
        size_t count = std::min(out.size(), front.size());
        std::copy_n(front.begin(), count, out.begin());
        if (responseQueue.size() > 1) {
            responseQueue.erase(responseQueue.begin());
        }
    }

    std::vector<std::vector<uint8_t>> responseQueue;
};

} // namespace mstar::test
