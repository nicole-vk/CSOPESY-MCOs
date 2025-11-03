#ifndef MC01_H
#define MC01_H

#include <string>

namespace MC01 {
    // Initializes and shuts down the scheduler system
    void init_module();
    void shutdown_module();

    // Main command handler (called by main console)
    bool handle_command(const std::string &cmd);

    // Returns a formatted snapshot of a process
    std::string screen_get_attached_output(const std::string &process_name);
}

#endif // MC01_H
