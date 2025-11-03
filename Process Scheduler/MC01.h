#ifndef MC01_H
#define MC01_H

#include <string>

namespace MC01 {
    void init_module();
    void shutdown_module();
    bool handle_command(const std::string &cmd);

    // Returns snapshot info of a specific process
    std::string screen_get_attached_output(const std::string &process_name);
}

#endif
