#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "mstar/flash/flash_table.hpp"
#include "mstar/flash/spi_bus.hpp"

namespace mstar::test {
// Stateful NAND, independent of SpiNand's decoder/control flow. It enforces
// WEL, erase-before-program, full cache replacement, and NAND's 1->0 behavior.
class NandModel final : public SpiBus {
public:
    FlashPart part = builtinFlashTable().front();
    uint8_t config = 0x10;
    uint8_t protection = 0x3E;
    uint8_t status = 0;
    bool wel = false;
    bool ignoreWren = false, ignoreUnlock = false, ignoreProtectionRestore = false;
    bool failErase = false, failProgram = false, corruptVerify = false;
    bool failRawRestore = false, backupDrift = false, uncorrectable = false;
    bool stuckBusy = false;
    bool busyAfterErase = false, busyAfterProgram = false;
    std::optional<uint8_t> failOpcode;
    bool failureAfterEffect = false;
    bool failureUsed = false;
    unsigned erases = 0, programs = 0;
    unsigned busyReads = 0;
    std::vector<std::vector<uint8_t>> commands;
    std::map<uint32_t, std::vector<uint8_t>> storage;
    std::set<uint32_t> erased;
    std::map<uint32_t, unsigned> rawReads;

    void select(NandEccProfile profile) {
        for (const auto& entry : builtinFlashTable()) if (entry.eccProfile == profile) part = entry;
        config = profile == NandEccProfile::Gd5f2gm7 ? 0x10 : 0x18;
        protection = part.vendor == "GigaDevice" ? 0x3E : 0x7C;
    }
    void mark(uint32_t block, uint32_t pageInBlock = 0, uint32_t byte = 0) {
        const auto page = block * part.geometry.pagesPerBlock + pageInBlock;
        auto data = contents(page);
        data.at(part.geometry.pageSize + byte) = 0;
        storage[page] = std::move(data);
    }
    std::vector<uint8_t> contents(uint32_t page) const {
        if (const auto it = storage.find(page); it != storage.end()) return it->second;
        std::vector<uint8_t> data(part.geometry.pageSize + part.geometry.oobSize, 0xFF);
        if (!erased.contains(page / part.geometry.pagesPerBlock)) {
            for (size_t i = 0; i < part.geometry.pageSize; ++i) data[i] = static_cast<uint8_t>(page * 17 + i);
        }
        return data;
    }
    Result<void> transaction(std::span<const uint8_t> tx, std::span<uint8_t> rx) override {
        commands.emplace_back(tx.begin(), tx.end());
        if (tx.empty()) return failure("empty command");
        const bool inject = failOpcode == tx[0] && !failureUsed;
        if (inject) failureUsed = true;
        if (inject && !failureAfterEffect) return failure("injected SPI failure");
        auto result = execute(tx, rx);
        if (inject) return failure("injected SPI failure after effect");
        return result;
    }
private:
    uint32_t loadedPage = 0;
    bool programLoaded = false;
    bool wasRaw = false;
    std::vector<uint8_t> cache;
    static std::unexpected<Error> failure(std::string message) {
        return std::unexpected(Error{ErrorCode::I2cTransfer, std::move(message)});
    }
    Result<void> execute(std::span<const uint8_t> tx, std::span<uint8_t> rx) {
        std::fill(rx.begin(), rx.end(), 0);
        auto row = [&] { return (uint32_t(tx[1]) << 16) | (uint32_t(tx[2]) << 8) | tx[3]; };
        switch (tx[0]) {
        case 0xFF: wel = false; status = 0; programLoaded = false; break;
        case 0x9F:
            std::copy_n(part.id.begin(), std::min(rx.size(), part.idLength), rx.begin());
            break;
        case 0x0F:
            if (tx.size() != 2 || rx.size() != 1) return failure("invalid GET FEATURE");
            if (tx[1] == 0xB0) rx[0] = config;
            if (tx[1] == 0xA0) rx[0] = protection;
            if (tx[1] == 0xC0) {
                rx[0] = status | (wel ? 2 : 0);
                if (stuckBusy || busyReads) rx[0] |= 1;
                if (busyReads) --busyReads;
            }
            break;
        case 0x1F:
            if (tx.size() != 3) return failure("invalid SET FEATURE");
            if (tx[1] == 0xB0) {
                if (failRawRestore && wasRaw && (tx[2] & 0x10)) return failure("injected raw ECC restore failure");
                config = tx[2];
                if (!(config & 0x10)) wasRaw = true;
            }
            if (tx[1] == 0xA0) {
                if (ignoreUnlock && tx[2] == 0) break;
                if (ignoreProtectionRestore && tx[2] != 0) break;
                protection = tx[2];
            }
            break;
        case 0x06: if (!ignoreWren) wel = true; break;
        case 0x04: wel = false; break;
        case 0x13:
            if (tx.size() != 4) return failure("invalid PAGE READ");
            loadedPage = row();
            cache = contents(loadedPage);
            if (!(config & 0x10) && backupDrift && ++rawReads[loadedPage] >= 3) cache[3] ^= 1;
            if (programs && corruptVerify) cache[0] ^= 1;
            status = uncorrectable && (config & 0x10) ? 0x20 : 0;
            wel = false;
            programLoaded = false;
            break;
        case 0x03: {
            if (tx.size() != 4) return failure("invalid CACHE READ");
            const auto column = (unsigned(tx[1]) << 8) | tx[2];
            if (column + rx.size() > cache.size()) return failure("cache read out of bounds");
            std::copy_n(cache.begin() + column, rx.size(), rx.begin());
            break;
        }
        case 0xD8: {
            if (tx.size() != 4 || !wel || protection) return failure("erase without WEL or unlocked flash");
            ++erases;
            const auto block = row() / part.geometry.pagesPerBlock;
            if (!failErase) {
                erased.insert(block);
                for (uint32_t p = block * part.geometry.pagesPerBlock; p < (block + 1) * part.geometry.pagesPerBlock; ++p)
                    storage.erase(p);
            }
            wel = false;
            status = failErase ? 4 : 0;
            busyReads = 1;
            if (busyAfterErase) stuckBusy = true;
            break;
        }
        case 0x02:
            if (tx.size() != part.geometry.pageSize + 3 || tx[1] || tx[2]) return failure("invalid PROGRAM LOAD");
            if (part.vendor == "Winbond" && !wel) return failure("Winbond load requires WEL");
            cache.assign(part.geometry.pageSize + part.geometry.oobSize, 0xFF);
            std::copy(tx.begin() + 3, tx.end(), cache.begin());
            programLoaded = true;
            break;
        case 0x10: {
            if (tx.size() != 4 || !wel || protection || !programLoaded) return failure("execute without valid loaded data/WEL");
            ++programs;
            const auto page = row();
            status = failProgram || !erased.contains(page / part.geometry.pagesPerBlock) ? 8 : 0;
            if (!status) {
                auto data = contents(page);
                for (size_t i = 0; i < data.size(); ++i) data[i] &= cache[i];
                storage[page] = std::move(data);
            }
            wel = false;
            programLoaded = false;
            busyReads = 1;
            if (busyAfterProgram) stuckBusy = true;
            break;
        }
        default: return failure("unexpected model opcode");
        }
        return {};
    }
};
} // namespace mstar::test
