// firesidecli: C++ command line for the FireWire route. See fireside/cli.h. Never run against the real radio yet; start with `list`, then `identify`.
#include <atomic>
#include <csignal>
#include <iostream>
#include "fireside/cli.h"

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint); std::signal(SIGTERM, on_sigint);
    fireside::CliEnv env = fireside::real_env();
    env.should_stop = [] { return g_stop.load(); };
    return fireside::run_cli(std::vector<std::string>(argv + 1, argv + argc), std::cout, std::cerr, env);
}
