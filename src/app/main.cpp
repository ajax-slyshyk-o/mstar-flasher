#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "mstar/isp/mstar_isp.hpp"

#ifdef MSTAR_ENABLE_FTDI
#include "mstar/transport/ftdi/ftdi_i2c.hpp"
#endif

namespace {

std::string hexBytes(std::span<const uint8_t> bytes) {
    std::string result;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) result += ' ';
        result += fmt::format("{:02X}", bytes[i]);
    }
    return result;
}

int runList() {
#ifdef MSTAR_ENABLE_FTDI
    auto devices = mstar::ftdi::FtdiI2c::enumerate();
    if (!devices) {
        fmt::println(stderr, "Failed to enumerate FTDI devices: {}", devices.error().message);
        return 1;
    }
    if (devices->empty()) {
        fmt::println("No FTDI programmer devices found.");
        return 0;
    }
    for (const auto& dev : *devices) {
        fmt::println("Programmer:");
        fmt::println("  FTDI device: {}", dev.kind);
        fmt::println("  Description: {}", dev.description);
        fmt::println("  Serial: {}", dev.serial);
        fmt::println("  Channel: {}", dev.location.empty() ? "-" : dev.location);
        if (dev.comPort) {
            fmt::println("  COM port: COM{}", *dev.comPort);
        }
        if (dev.inUse) {
            fmt::println(
                "  Note: held open by another process/driver (fields above may be incomplete)");
        }
        fmt::println("");
    }
    return 0;
#else
    fmt::println("No programmer backends are enabled in this build.");
    return 0;
#endif
}

int runProbe(const std::string& serial, uint8_t ispAddress, uint32_t i2cClockHz) {
#ifdef MSTAR_ENABLE_FTDI
    mstar::ProgrammerSelector selector;
    if (!serial.empty()) {
        selector.serial = serial;
    }

    auto i2c = mstar::ftdi::FtdiI2c::open(selector);
    if (!i2c) {
        fmt::println(stderr, "Failed to open FTDI I2C channel: {}", i2c.error().message);
        return 1;
    }

    if (auto result = i2c->setClock(i2cClockHz); !result) {
        fmt::println(stderr, "Failed to set I2C clock: {}", result.error().message);
        return 1;
    }

    fmt::println("Target:");
    fmt::println("  MStar ISP address: {:#x}", ispAddress);

    mstar::MstarIsp isp(*i2c, ispAddress);

    if (auto result = isp.enter(); !result) {
        fmt::println("  ISP activation: FAILED ({})", result.error().message);
        return 1;
    }
    fmt::println("  ISP activation: OK");
    fmt::println("");

    std::array<uint8_t, 1> readJedecId{0x9F};
    std::array<uint8_t, 3> jedecId{};
    auto result = isp.transaction(readJedecId, jedecId);

    // Best-effort: release the SoC from ISP mode whether or not the JEDEC
    // ID read below succeeded. probe must never leave the target stuck in
    // a debug state.
    if (auto leaveResult = isp.leave(); !leaveResult) {
        fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                      leaveResult.error().message);
    }

    if (!result) {
        fmt::println(stderr, "Failed to read flash JEDEC ID: {}", result.error().message);
        return 1;
    }

    fmt::println("Flash:");
    fmt::println("  JEDEC ID: {}", hexBytes(jedecId));
    return 0;
#else
    (void)serial;
    (void)ispAddress;
    (void)i2cClockHz;
    fmt::println("No programmer backends are enabled in this build.");
    return 1;
#endif
}

// Milestone 2 hardware validation (see docs/BLUEPRINT.md, "Test B - I2C
// electrical activity"): a single-byte write with nothing above I2cMaster
// involved, so START/address/ACK/STOP can be inspected on a logic analyzer.
int runI2cTest(const std::string& serial, uint8_t i2cAddress, uint32_t i2cClockHz) {
#ifdef MSTAR_ENABLE_FTDI
    mstar::ProgrammerSelector selector;
    if (!serial.empty()) {
        selector.serial = serial;
    }

    auto i2c = mstar::ftdi::FtdiI2c::open(selector);
    if (!i2c) {
        fmt::println(stderr, "Failed to open FTDI I2C channel: {}", i2c.error().message);
        return 1;
    }

    if (auto result = i2c->setClock(i2cClockHz); !result) {
        fmt::println(stderr, "Failed to set I2C clock: {}", result.error().message);
        return 1;
    }

    fmt::println("Probing I2C address {:#x} at {} Hz...", i2cAddress, i2cClockHz);
    fmt::println("Expect on a logic analyzer: START, address+W, ACK/NACK, STOP.");

    std::array<uint8_t, 1> probeByte{0x00};
    auto result = i2c->write(i2cAddress, probeByte);
    if (!result) {
        fmt::println("No ACK ({})", result.error().message);
        return 1;
    }

    fmt::println("ACK received.");
    return 0;
#else
    (void)serial;
    (void)i2cAddress;
    (void)i2cClockHz;
    fmt::println("No programmer backends are enabled in this build.");
    return 1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"mstar-flasher: in-system programmer for MStar/SigmaStar SPI-NAND/NOR"};
    app.require_subcommand(1);

    app.add_subcommand("list", "List detected programmer devices")
        ->callback([] { std::exit(runList()); });

    auto* probe = app.add_subcommand("probe", "Identify the attached flash without modifying it");
    static std::string serial;
    static uint32_t ispAddress = 0x49;
    static uint32_t i2cClockHz = 100000;
    probe->add_option("--serial", serial, "Select programmer by serial number");
    probe->add_option("--i2c-address", ispAddress, "MStar ISP I2C address")->default_val(0x49);
    probe->add_option("--i2c-clock", i2cClockHz, "I2C clock in Hz")->default_val(100000);
    probe->callback([] {
        std::exit(runProbe(serial, static_cast<uint8_t>(ispAddress), i2cClockHz));
    });

    auto* i2cTest = app.add_subcommand(
        "i2c-test", "Send a single I2C write to verify electrical activity on a logic analyzer");
    static std::string i2cTestSerial;
    static uint32_t i2cTestAddress = 0x49;
    static uint32_t i2cTestClockHz = 100000;
    i2cTest->add_option("--serial", i2cTestSerial, "Select programmer by serial number");
    i2cTest->add_option("--i2c-address", i2cTestAddress, "I2C address to probe")
        ->default_val(0x49);
    i2cTest->add_option("--i2c-clock", i2cTestClockHz, "I2C clock in Hz")->default_val(100000);
    i2cTest->callback([] {
        std::exit(runI2cTest(i2cTestSerial, static_cast<uint8_t>(i2cTestAddress), i2cTestClockHz));
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
