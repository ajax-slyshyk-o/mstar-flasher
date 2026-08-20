#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "mstar/transport/i2c_master.hpp"

namespace mstar::test {

enum class TransactionKind { Write, Read, WriteRead };

struct RecordedTransaction {
    TransactionKind kind;
    uint8_t address;
    std::vector<uint8_t> tx;
    std::vector<uint8_t> rx; // requested rx contents for Read/WriteRead
};

/// Records every transaction it is asked to perform and plays back
/// canned responses for read/writeRead, so protocol layers can be
/// tested without hardware.
class MockI2cMaster final : public I2cMaster {
public:
    Result<void> setClock(uint32_t hz) override {
        lastClockHz = hz;
        return {};
    }

    Result<void> write(uint8_t address, std::span<const uint8_t> data) override {
        if (failNextWrite) {
            failNextWrite = false;
            return std::unexpected(Error{ErrorCode::I2cNack, "mock write NACK"});
        }
        transactions.push_back(RecordedTransaction{
            TransactionKind::Write, address,
            std::vector<uint8_t>(data.begin(), data.end()), {}});
        return {};
    }

    Result<void> read(uint8_t address, std::span<uint8_t> data) override {
        if (failNextRead) {
            failNextRead = false;
            return std::unexpected(Error{ErrorCode::I2cTransfer, "mock read failure"});
        }
        fillFromQueue(data);
        transactions.push_back(RecordedTransaction{
            TransactionKind::Read, address, {},
            std::vector<uint8_t>(data.begin(), data.end())});
        return {};
    }

    Result<void> writeRead(
        uint8_t address,
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) override {
        if (failNextWriteRead) {
            failNextWriteRead = false;
            transactions.push_back(RecordedTransaction{
                TransactionKind::WriteRead, address,
                std::vector<uint8_t>(tx.begin(), tx.end()), {}});
            return std::unexpected(Error{ErrorCode::I2cTransfer, "mock writeRead failure"});
        }
        fillFromQueue(rx);
        transactions.push_back(RecordedTransaction{
            TransactionKind::WriteRead, address,
            std::vector<uint8_t>(tx.begin(), tx.end()),
            std::vector<uint8_t>(rx.begin(), rx.end())});
        return {};
    }

    /// Queues bytes to be returned by the next read/writeRead call(s).
    void queueResponse(std::vector<uint8_t> bytes) {
        responseQueue.push_back(std::move(bytes));
    }

    std::vector<RecordedTransaction> transactions;
    uint32_t lastClockHz = 0;

    bool failNextWrite = false;
    bool failNextRead = false;
    bool failNextWriteRead = false;

private:
    void fillFromQueue(std::span<uint8_t> out) {
        if (responseQueue.empty()) {
            return;
        }
        auto& front = responseQueue.front();
        size_t count = std::min(out.size(), front.size());
        std::copy_n(front.begin(), count, out.begin());
        responseQueue.erase(responseQueue.begin());
    }

    std::vector<std::vector<uint8_t>> responseQueue;
};

} // namespace mstar::test
