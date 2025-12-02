#ifndef FINAL_H
#define FINAL_H

#include <string>
#include <cstdint>

namespace CSOPESY {
    void init_module();
    void shutdown_module();
    bool handle_command(const std::string &cmd);
    std::string screen_get_attached_output(const std::string &process_name);
    
    // Memory management functions
    void init_memory_manager();
    
    // Memory statistics
    struct MemoryStats {
        uint64_t total_memory;
        uint64_t used_memory;
        uint64_t free_memory;
        uint64_t num_pages_in;
        uint64_t num_pages_out;
        uint32_t total_frames;
        uint32_t used_frames;
        uint32_t free_frames;
    };
    
    MemoryStats get_memory_stats();
}

#endif