#include <cstdint>
#include <iostream>

#include <CLI/CLI.hpp>

namespace {

int runList() {
    std::cout << "No programmer backends are enabled in this build.\n";
    return 0;
}

int runProbe(const std::string& serial, uint8_t ispAddress, uint32_t i2cClockHz) {
    (void)serial;
    (void)ispAddress;
    (void)i2cClockHz;
    std::cout << "No programmer backends are enabled in this build.\n";
    return 1;
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

    CLI11_PARSE(app, argc, argv);
    return 0;
}
