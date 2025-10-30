#ifndef MC01_H
#define MC01_H

#include <string>

namespace MC01 {

// initialize internal structures (module load)
void init_module();

// shutdown module (stop threads, flush logs)
void shutdown_module();

// handle a single command string from the main CLI.
// Returns true if MC01 recognized/handled the command, false otherwise.
bool handle_command(const std::string &cmd);

// called periodically by main display system to fetch/format logs for an attached process.
// If attached process name is empty -> returns empty string.
// Format is multi-line info for display inside a "screen".
std::string screen_get_attached_output(const std::string &process_name);

} // namespace MC01

#endif // MC01_H