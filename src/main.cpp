#include <cstdlib>
#include <iostream>

#include "client.hpp"
#include "config.hpp"
#include "metrics.hpp"

int main(int argc, char** argv) {
    ehlt::Config cfg;
    bool show_help = false;
    std::string err;

    if (!ehlt::parse_config(argc, argv, cfg, show_help, err)) {
        std::cerr << "error: " << err << "\n\n";
        ehlt::print_usage(argv[0]);
        return 2;
    }
    if (show_help) {
        ehlt::print_usage(argv[0]);
        return 0;
    }

    std::cout << ehlt::describe(cfg) << "\n";

    ehlt::Metrics metrics;
    try {
        ehlt::Client client(std::move(cfg), metrics);
        return client.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
