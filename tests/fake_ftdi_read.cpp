// Test-only FtdiI2c replacement for CLI read tests. No USB libraries are linked.
// --serial selects a deterministic scenario; only this test executable uses it.
#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <fmt/format.h>
#include "nand_model.hpp"

namespace mstar::ftdi {
struct FtdiI2c::Impl {
    std::unique_ptr<test::NandModel> model;
    std::vector<uint8_t> pending;
    bool executed = false;
    std::string scenario;
    uint8_t opcode = 0;
    uint8_t featureAddress = 0;
    uint8_t config = 0x10;
    unsigned configWrites = 0;
    unsigned rawPageReads = 0;
    unsigned mainReadsBeforeErase = 0;
    uint32_t page = 0;
    bool pageLoaded = false;

    Result<void> reply(std::span<uint8_t> data) {
        std::fill(data.begin(), data.end(), 0);
        if (opcode == 0x9F && data.size() >= 2) {
            data[0] = 0xC8; // Built-in GD5F2GM7, 2048-byte pages.
            data[1] = 0x92;
            if (scenario.starts_with("gv-") || scenario.starts_with("kv-")) {
                data[0] = 0xEF;
                data[1] = 0xAA;
                if (data.size() > 2) data[2] = scenario.starts_with("gv-") ? 0x21 : 0x22;
            } else if (scenario == "gm9-corrected") {
                data[1] = 0x91;
                if (data.size() > 2) data[2] = 1;
            }
        } else if (opcode == 0x0F && !data.empty()) {
            if (featureAddress == 0xB0) {
                data[0] = scenario == "config-lost" && pageLoaded ? 0 : config;
            } else if (featureAddress == 0xC0) {
                const bool ecc = (scenario == "ecc" || scenario == "ecc-then-transport") && page == 1;
                if (pageLoaded && (ecc || scenario == "ecc-all")) data[0] = 0x20;
                if (pageLoaded) {
                    if (scenario == "mixed") data[0] = page == 0 ? 0 : page == 1 ? 0x10 : 0x30;
                    if (scenario == "gv-corrected" || scenario == "kv-corrected" ||
                        scenario == "gm9-corrected" || scenario == "kv-invalid" ||
                        scenario == "extended-error") data[0] = 0x10;
                    if (scenario == "gv-invalid" || scenario == "kv-threshold") data[0] = 0x30;
                }
            } else if (featureAddress == 0xF0 || featureAddress == 0x30) {
                if (scenario == "extended-error")
                    return std::unexpected(Error{ErrorCode::I2cTransfer, "injected ECC detail read error"});
                data[0] = scenario == "kv-invalid" ? 0xF0 : scenario.starts_with("kv-") ? 0x30 :
                          scenario == "gm9-corrected" ? 0x20 : 0;
            }
        } else if (opcode == 0x03) {
            for (std::size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(page * 17 + i);
        }
        return {};
    }
};

FtdiI2c::FtdiI2c(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FtdiI2c::FtdiI2c(FtdiI2c&&) noexcept = default;
FtdiI2c& FtdiI2c::operator=(FtdiI2c&&) noexcept = default;
FtdiI2c::~FtdiI2c() {
    if (impl_ && impl_->model) {
        const auto& m = *impl_->model;
        fmt::println(stderr, "MODEL erases={} programs={} config={:02X} protection={:02X} wel={}",
                     m.erases, m.programs, m.config, m.protection, m.wel ? 1 : 0);
        fmt::println(stderr, "MODEL_READS raw_pages={} main_before_erase={}",
                     impl_->rawPageReads, impl_->mainReadsBeforeErase);
        if (impl_->scenario == "write-config-ff" || impl_->scenario == "write-config-preserve") {
            // Independent expected memory map for the CLI config fixture:
            // abc at 0 and 128 KiB; 128 KiB+1 of B at 512 KiB. Gaps untouched.
            bool correct = m.erased == std::set<uint32_t>{0, 1, 4, 5};
            const bool preserve = impl_->scenario == "write-config-preserve";
            for (uint32_t page = 0; page < 7 * 64; ++page) {
                const auto data = m.contents(page);
                for (uint32_t i = 0; i < 2048; ++i) {
                    const uint32_t address = page * 2048 + i;
                    uint8_t expected = static_cast<uint8_t>(page * 17 + i);
                    if (!preserve && m.erased.contains(page / 64)) expected = 0xFF;
                    if (address < 3) expected = "abc"[address];
                    if (address >= 131072 && address < 131075) expected = "abc"[address - 131072];
                    if (address >= 524288 && address < 655361) expected = 'B';
                    if (data[i] != expected) correct = false;
                }
            }
            fmt::println(stderr, "CONFIG_AUDIT={}", correct ? "ok" : "failed");
        }
    }
}

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    return std::vector<ProgrammerInfo>{};
}
Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector& selector) {
    auto impl = std::make_unique<Impl>();
    impl->scenario = selector.serial.value_or(selector.firstMatch ? "write-ok" : "clean");
    if (impl->scenario.starts_with("write-")) {
        impl->model = std::make_unique<test::NandModel>();
        auto& m = *impl->model;
        const auto& s = impl->scenario;
        if (s == "write-gv") m.select(NandEccProfile::W25n01gv);
        if (s == "write-bad-first") m.mark(0);
        if (s == "write-bad-second") m.mark(0, 1);
        if (s == "write-config-bad-later") m.mark(4);
        if (s == "write-config-bad-gap") m.mark(2);
        if (s == "write-config-backup-later") m.failReadPage = 4 * 64 + 3;
        if (s == "write-config-preserve-later") m.failReadPage = 5 * 64 + 3;
        if (s == "write-marker-error") m.failOpcode = 0x03;
        if (s == "write-wel") m.ignoreWren = true;
        if (s == "write-protected") m.ignoreUnlock = true;
        if (s == "write-erase-fail") m.failErase = true;
        if (s == "write-program-fail") m.failProgram = true;
        if (s == "write-mismatch") m.corruptVerify = true;
        if (s == "write-restore-fail") m.ignoreProtectionRestore = true;
        if (s == "write-raw-restore-fail") m.failRawRestore = true;
        if (s == "write-backup-drift") m.backupDrift = true;
        if (s == "write-backup-ecc") m.uncorrectable = true;
        if (s == "write-load-error") m.failOpcode = 0x02;
        if (s == "write-execute-error") { m.failOpcode = 0x10; m.failureAfterEffect = true; }
        if (s == "write-preloaded") {
            for (uint32_t p = 0; p < 64; ++p) {
                auto data = m.contents(p);
                std::fill_n(data.begin(), m.part.geometry.pageSize, 0x41);
                m.storage[p] = std::move(data);
            }
        }
    }
    if (impl->scenario.starts_with("gv-") || impl->scenario.starts_with("kv-") ||
        impl->scenario == "gm9-corrected") impl->config = 0x18;
    if (impl->scenario == "config-disabled" || impl->scenario == "config-ignored" ||
        impl->scenario == "config-write-error" || impl->scenario == "restore-error" ||
        impl->scenario == "restore-mismatch") impl->config = 0x01; // preserve QE
    return FtdiI2c(std::move(impl));
}
Result<void> FtdiI2c::setClock(uint32_t) { return {}; }
Result<void> FtdiI2c::write(uint8_t, std::span<const uint8_t> data) {
    if (impl_->model) {
        if (data.size() >= 2 && data[0] == 0x10) {
            impl_->pending.assign(data.begin() + 1, data.end());
            impl_->executed = false;
        } else if (data.size() == 1 && data[0] == 0x12 && !impl_->executed && !impl_->pending.empty()) {
            impl_->executed = true;
            return impl_->model->transaction(impl_->pending, {});
        }
        return {};
    }
    if (data.size() >= 2 && data[0] == 0x10) {
        impl_->opcode = data[1];
        if (impl_->opcode == 0x0F && data.size() == 3) impl_->featureAddress = data[2];
        if (impl_->opcode == 0x1F && data.size() == 4 && data[2] == 0xB0) {
            ++impl_->configWrites;
            if (impl_->configWrites == 2 && impl_->scenario == "restore-error")
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected restore error"});
            if (impl_->scenario != "config-ignored" &&
                !(impl_->configWrites == 2 && impl_->scenario == "restore-mismatch"))
                impl_->config = data[3];
            if (impl_->configWrites == 1 && impl_->scenario == "config-write-error")
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected configuration write error"});
        }
        if (impl_->opcode == 0xFF) impl_->pageLoaded = false;
        if (impl_->opcode == 0x13 && data.size() == 5) {
            impl_->page = (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 8) | data[4];
            impl_->pageLoaded = true;
            if ((impl_->scenario == "transport" && impl_->page == 1) ||
                (impl_->scenario == "ecc-then-transport" && impl_->page == 2)) {
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected transport error"});
            }
        }
    }
    return {};
}
Result<void> FtdiI2c::read(uint8_t, std::span<uint8_t> data) {
    return impl_->reply(data);
}
Result<void> FtdiI2c::writeRead(uint8_t, std::span<const uint8_t>, std::span<uint8_t> rx) {
    if (impl_->model) {
        if (!impl_->pending.empty() && impl_->pending[0] == 0x03) {
            const auto pageSize = impl_->model->part.geometry.pageSize;
            if (rx.size() > pageSize) ++impl_->rawPageReads;
            if (rx.size() == pageSize && impl_->model->erases == 0)
                ++impl_->mainReadsBeforeErase;
        }
        impl_->executed = true;
        return impl_->model->transaction(impl_->pending, rx);
    }
    return impl_->reply(rx);
}
} // namespace mstar::ftdi
