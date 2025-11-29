#include "final.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>
#include <map>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstring>

extern void setMessage(const std::string &s);
using namespace std;

namespace CSOPESY {

    
    struct Config {
        uint32_t num_cpu = 4;
        string scheduler = "rr";
        uint64_t quantum_cycles = 5;
        uint64_t batch_process_freq = 1;
        uint32_t min_ins = 1000;
        uint32_t max_ins = 2000;
        uint64_t delays_per_exec = 0;
        
        // memory parameters
        uint64_t max_overall_mem = 16384;  // 2^14 bytes default
        uint64_t mem_per_frame = 256;      // 2^8 bytes default
        uint64_t min_mem_per_proc = 256;
        uint64_t max_mem_per_proc = 1024;
    } config;

    
    struct Frame {
        bool occupied = false;
        int process_id = -1;
        uint32_t page_number = 0;
        vector<uint16_t> data;
        
        Frame() {
            data.resize(256, 0);
        }
    };

    struct Page {
        uint32_t page_number;
        int frame_number = -1;
        bool in_memory = false;
        bool dirty = false;
    };

    struct PageTable {
        vector<Page> pages;
        uint64_t total_size;
        
        PageTable(uint64_t size, uint64_t frame_size) {
            total_size = size;
            uint32_t num_pages = (size + frame_size - 1) / frame_size;
            pages.resize(num_pages);
            for (uint32_t i = 0; i < num_pages; i++) {
                pages[i].page_number = i;
            }
        }
    };

    vector<Frame> physical_memory;
    mutex memory_mutex;
    atomic<uint64_t> num_pages_paged_in(0);
    atomic<uint64_t> num_pages_paged_out(0);
    uint32_t next_frame_to_evict = 0;

    string BACKING_STORE_FILE = "csopesy-backing-store.txt";

    
    atomic<bool> initialized(false);
    atomic<bool> generator_running(false);
    atomic<bool> stop_all(false);

    enum ProcState { P_READY, P_RUNNING, P_SLEEPING, P_FINISHED, P_ERROR };

    struct Instr {
        string opcode;
        vector<string> args;
    };

    struct Process {
        int pid;
        string name;
        vector<Instr> instructions;
        size_t ip = 0;
        map<string, uint16_t> vars;
        vector<string> logs;
        ProcState state = P_READY;
        uint64_t wake_tick = 0;
        mutex m;
        chrono::system_clock::time_point start_time;
        int current_core = -1;
        
        // memory management
        uint64_t memory_size = 0;
        PageTable* page_table = nullptr;
        bool has_memory_error = false;
        string error_message;
        chrono::system_clock::time_point error_time;
        
        ~Process() {
            if (page_table) delete page_table;
        }
    };

    mutex processes_mutex;
    map<int, shared_ptr<Process>> processes_by_pid;
    map<string, shared_ptr<Process>> processes_by_name;
    int next_pid = 1;

    deque<int> ready_queue;
    mutex ready_mutex;
    condition_variable ready_cv;

    uint64_t cpu_tick = 0;
    mutex tick_mutex;

    vector<thread> cpu_workers;
    thread scheduler_thread;

    mutex stats_mutex;
    uint64_t total_executed_instructions = 0;
    uint64_t total_processes_created = 0;

    string attached_process = "";
    string last_screen_s_process = "";

    mt19937 rng((uint32_t)chrono::high_resolution_clock::now().time_since_epoch().count());

    vector<char> core_busy;
    atomic<uint64_t> busy_core_count(0);
    atomic<uint64_t> total_busy_ticks(0);
    atomic<uint64_t> total_ticks_elapsed(0);


    static string trim(const string &s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static vector<string> split_space(const string &s) {
        vector<string> out;
        string tmp;
        istringstream iss(s);
        while (iss >> tmp) out.push_back(tmp);
        return out;
    }

    static uint32_t clamp_uint16(int64_t v) {
        if (v < 0) return 0;
        if (v > 0xFFFF) return 0xFFFF;
        return (uint32_t)v;
    }

    static string format_time(const chrono::system_clock::time_point &tp) {
        time_t t = chrono::system_clock::to_time_t(tp);
        struct tm tm;
        #ifdef _WIN32
            localtime_s(&tm, &t);
        #else
            localtime_r(&t, &tm);
        #endif
        char b[64];
        strftime(b, sizeof(b), "(%m/%d/%Y %I:%M:%S%p)", &tm);
        return string(b);
    }

    static bool is_power_of_two(uint64_t n) {
        return n > 0 && (n & (n - 1)) == 0;
    }

    static bool is_valid_memory_size(uint64_t size) {
        return size >= 64 && size <= 65536 && is_power_of_two(size);
    }

  
    void init_memory_manager() {
        lock_guard<mutex> lock(memory_mutex);
        
        uint32_t total_frames = config.max_overall_mem / config.mem_per_frame;
        physical_memory.clear();
        physical_memory.resize(total_frames);
        
        for (auto &frame : physical_memory) {
            frame.data.resize(config.mem_per_frame / 2, 0);
        }
        
        next_frame_to_evict = 0;
        num_pages_paged_in = 0;
        num_pages_paged_out = 0;
        
        ofstream backing_store(BACKING_STORE_FILE, ios::trunc);
        backing_store.close();
    }

    int find_free_frame() {
        lock_guard<mutex> lock(memory_mutex);
        for (size_t i = 0; i < physical_memory.size(); i++) {
            if (!physical_memory[i].occupied) {
                return (int)i;
            }
        }
        return -1;
    }

    void write_page_to_backing_store(int frame_idx) {
        if (frame_idx < 0 || frame_idx >= (int)physical_memory.size()) return;
        
        auto &frame = physical_memory[frame_idx];
        if (!frame.occupied) return;
        
        ofstream backing_store(BACKING_STORE_FILE, ios::app);
        backing_store << "FRAME:" << frame_idx 
                     << " PID:" << frame.process_id
                     << " PAGE:" << frame.page_number << "\n";
        
        for (size_t i = 0; i < frame.data.size(); i++) {
            backing_store << frame.data[i];
            if (i < frame.data.size() - 1) backing_store << ",";
        }
        backing_store << "\n";
        backing_store.close();
        
        num_pages_paged_out++;
    }

    int evict_page_fifo() {
        lock_guard<mutex> lock(memory_mutex);
        
        if (physical_memory.empty()) return -1;
        
        uint32_t start = next_frame_to_evict;
        for (uint32_t i = 0; i < physical_memory.size(); i++) {
            uint32_t idx = (start + i) % physical_memory.size();
            if (physical_memory[idx].occupied) {
                write_page_to_backing_store(idx);
                
                int evicted_pid = physical_memory[idx].process_id;
                uint32_t evicted_page = physical_memory[idx].page_number;
                
                lock_guard<mutex> plock(processes_mutex);
                if (processes_by_pid.count(evicted_pid)) {
                    auto proc = processes_by_pid[evicted_pid];
                    if (proc->page_table && evicted_page < proc->page_table->pages.size()) {
                        proc->page_table->pages[evicted_page].in_memory = false;
                        proc->page_table->pages[evicted_page].frame_number = -1;
                    }
                }
                
                physical_memory[idx].occupied = false;
                physical_memory[idx].process_id = -1;
                next_frame_to_evict = (idx + 1) % physical_memory.size();
                return idx;
            }
        }
        return -1;
    }

    bool load_page_to_memory(shared_ptr<Process> p, uint32_t page_num) {
        if (!p->page_table || page_num >= p->page_table->pages.size()) {
            return false;
        }
        
        auto &page = p->page_table->pages[page_num];
        if (page.in_memory) return true;
        
        int frame_idx = find_free_frame();
        if (frame_idx == -1) {
            frame_idx = evict_page_fifo();
            if (frame_idx == -1) return false;
        }
        
        lock_guard<mutex> lock(memory_mutex);
        physical_memory[frame_idx].occupied = true;
        physical_memory[frame_idx].process_id = p->pid;
        physical_memory[frame_idx].page_number = page_num;
        
        page.in_memory = true;
        page.frame_number = frame_idx;
        num_pages_paged_in++;
        
        return true;
    }

    
    static Instr make_PRINT(const string &msg) { 
        Instr i; i.opcode = "PRINT"; i.args = {msg}; return i; 
    }
    
    static Instr make_ADD(const string &d, const string &a, const string &b) { 
        Instr i; i.opcode = "ADD"; i.args = {d, a, b}; return i; 
    }
    
    static Instr make_SUB(const string &d, const string &a, const string &b) { 
        Instr i; i.opcode = "SUBTRACT"; i.args = {d, a, b}; return i; 
    }
    
    static Instr make_DECLARE(const string &v, const string &val) { 
        Instr i; i.opcode = "DECLARE"; i.args = {v, val}; return i; 
    }
    
    static Instr make_SLEEP(const string &ticks) { 
        Instr i; i.opcode = "SLEEP"; i.args = {ticks}; return i; 
    }
    
    // MCO2: New instructions
    static Instr make_READ(const string &var, const string &addr) {
        Instr i; i.opcode = "READ"; i.args = {var, addr}; return i;
    }
    
    static Instr make_WRITE(const string &addr, const string &val) {
        Instr i; i.opcode = "WRITE"; i.args = {addr, val}; return i;
    }

    static Instr make_FOR(uint32_t repeats, const string &encoded_inner){
        Instr i; i.opcode="FOR"; i.args={to_string(repeats), encoded_inner}; return i;
    }

    static Instr parse_encoded_instr(const string &enc) {
        Instr out;
        if (enc.empty()) return out;
        vector<string> parts;
        string token;
        istringstream iss(enc);
        while (getline(iss, token, ':')) parts.push_back(token);
        if (parts.empty()) return out;
        string op = parts[0];
        if (op == "PRINT") {
            out.opcode = "PRINT";
            size_t pos = enc.find(':');
            if (pos != string::npos) out.args = {enc.substr(pos+1)};
            else out.args = {""};
        } else if (op == "ADD" && parts.size() >= 4) {
            out.opcode = "ADD";
            out.args = {parts[1], parts[2], parts[3]};
        } else if (op == "SUBTRACT" && parts.size() >= 4) {
            out.opcode = "SUBTRACT";
            out.args = {parts[1], parts[2], parts[3]};
        } else if (op == "DECLARE" && parts.size() >= 3) {
            out.opcode = "DECLARE";
            out.args = {parts[1], parts[2]};
        } else if (op == "SLEEP" && parts.size() >= 2) {
            out.opcode = "SLEEP";
            out.args = {parts[1]};
        } else if (op == "READ" && parts.size() >= 3) {
            out.opcode = "READ";
            out.args = {parts[1], parts[2]};
        } else if (op == "WRITE" && parts.size() >= 3) {
            out.opcode = "WRITE";
            out.args = {parts[1], parts[2]};
        } else {
            out.opcode = "PRINT";
            out.args = {enc};
        }
        return out;
    }

    static vector<Instr> decode_inner_instructions(const string &encoded_inner) {
        vector<Instr> out;
        string token;
        istringstream iss(encoded_inner);
        while (getline(iss, token, ';')) {
            token = trim(token);
            if (token.empty()) continue;
            out.push_back(parse_encoded_instr(token));
        }
        return out;
    }

    
    static vector<Instr> random_instructions_for(const string &pname) {
        uniform_int_distribution<int> d(config.min_ins, config.max_ins);
        int n = d(rng);
        if (n < 2) n = 2;
        vector<Instr> out;
        uniform_int_distribution<int> op_choice(0, 8);
        uniform_int_distribution<int> smallval(1,10);
        uniform_int_distribution<int> sleepval(1,5);
        uniform_int_distribution<int> for_inner_len(1,4);
        uniform_int_distribution<int> for_repeats(1,4);
        uniform_int_distribution<int> mem_addr(0x1000, 0x5000);
        
        for (int i = 0; i < n; ++i) {
            int choice = op_choice(rng);
            if (choice == 0) {
                out.push_back(make_PRINT("Hello world from " + pname + "!"));
            } else if (choice == 1) {
                out.push_back(make_ADD("x", "x", to_string(smallval(rng))));
            } else if (choice == 2) {
                out.push_back(make_SUB("x", "x", to_string(smallval(rng))));
            } else if (choice == 3) {
                string var = string("v") + to_string(smallval(rng));
                out.push_back(make_DECLARE(var, to_string(smallval(rng))));
            } else if (choice == 4) {
                out.push_back(make_SLEEP(to_string((int)sleepval(rng))));
            } else if (choice == 5) {
                // FOR loop
                int inner_count = for_inner_len(rng);
                vector<string> inner_enc;
                for (int j=0;j<inner_count;j++) {
                    int ic = op_choice(rng) % 4;
                    if (ic == 0) inner_enc.push_back("PRINT:Hello from FOR");
                    else if (ic == 1) inner_enc.push_back("ADD:x:x:" + to_string(smallval(rng)));
                    else if (ic == 2) inner_enc.push_back("SUBTRACT:x:x:" + to_string(smallval(rng)));
                    else inner_enc.push_back("SLEEP:" + to_string((int)sleepval(rng)));
                }
                string enc;
                for (size_t k=0;k<inner_enc.size();k++){
                    if (k) enc += ";";
                    enc += inner_enc[k];
                }
                int repeats = for_repeats(rng);
                out.push_back(make_FOR((uint32_t)repeats, enc));
            } else if (choice == 6) {
                // READ 
                stringstream ss;
                ss << "0x" << hex << mem_addr(rng);
                out.push_back(make_READ("temp_var", ss.str()));
            } else if (choice == 7) {
                // WRITE
                stringstream ss;
                ss << "0x" << hex << mem_addr(rng);
                out.push_back(make_WRITE(ss.str(), to_string(smallval(rng))));
            } else {
                out.push_back(make_PRINT("Hello world from " + pname + "!"));
            }
        }
        return out;
    }

    // parse user-defined instructions
    vector<Instr> parse_user_instructions(const string &instr_str) {
        vector<Instr> out;
        stringstream ss(instr_str);
        string token;
        
        while (getline(ss, token, ';')) {
            token = trim(token);
            if (token.empty()) continue;
            
            auto parts = split_space(token);
            if (parts.empty()) continue;
            
            string op = parts[0];
            if (op == "DECLARE" && parts.size() >= 3) {
                out.push_back(make_DECLARE(parts[1], parts[2]));
            } else if (op == "ADD" && parts.size() >= 4) {
                out.push_back(make_ADD(parts[1], parts[2], parts[3]));
            } else if (op == "SUBTRACT" && parts.size() >= 4) {
                out.push_back(make_SUB(parts[1], parts[2], parts[3]));
            } else if (op == "PRINT") {
                size_t first_quote = token.find('"');
                size_t last_quote = token.rfind('"');
                string msg;
                if (first_quote != string::npos && last_quote != string::npos && first_quote != last_quote) {
                    msg = token.substr(first_quote + 1, last_quote - first_quote - 1);
                } else if (parts.size() >= 2) {
                    msg = token.substr(token.find(parts[1]));
                }
                out.push_back(make_PRINT(msg));
            } else if (op == "READ" && parts.size() >= 3) {
                out.push_back(make_READ(parts[1], parts[2]));
            } else if (op == "WRITE" && parts.size() >= 3) {
                out.push_back(make_WRITE(parts[1], parts[2]));
            } else if (op == "SLEEP" && parts.size() >= 2) {
                out.push_back(make_SLEEP(parts[1]));
            }
        }
        return out;
    }

    // memory access helpers
    uint64_t hex_to_uint64(const string &hex_str) {
        uint64_t value = 0;
        stringstream ss;
        ss << hex << hex_str;
        ss >> value;
        return value;
    }

    bool is_valid_memory_address(shared_ptr<Process> p, uint64_t addr) {
        if (!p->page_table) return false;
        return addr < p->memory_size;
    }

    uint16_t read_memory(shared_ptr<Process> p, uint64_t addr) {
        if (!is_valid_memory_address(p, addr)) {
            p->has_memory_error = true;
            stringstream ss;
            ss << "0x" << hex << addr;
            p->error_message = ss.str();
            p->error_time = chrono::system_clock::now();
            return 0;
        }
        
        uint32_t page_num = addr / config.mem_per_frame;
        uint32_t offset = (addr % config.mem_per_frame) / 2;
        
        if (!load_page_to_memory(p, page_num)) {
            p->has_memory_error = true;
            stringstream ss;
            ss << "0x" << hex << addr;
            p->error_message = ss.str();
            p->error_time = chrono::system_clock::now();
            return 0;
        }
        
        auto &page = p->page_table->pages[page_num];
        int frame_idx = page.frame_number;
        
        if (frame_idx >= 0 && frame_idx < (int)physical_memory.size()) {
            lock_guard<mutex> lock(memory_mutex);
            if (offset < physical_memory[frame_idx].data.size()) {
                return physical_memory[frame_idx].data[offset];
            }
        }
        
        return 0;
    }

    void write_memory(shared_ptr<Process> p, uint64_t addr, uint16_t value) {
        if (!is_valid_memory_address(p, addr)) {
            p->has_memory_error = true;
            stringstream ss;
            ss << "0x" << hex << addr;
            p->error_message = ss.str();
            p->error_time = chrono::system_clock::now();
            return;
        }
        
        uint32_t page_num = addr / config.mem_per_frame;
        uint32_t offset = (addr % config.mem_per_frame) / 2;
        
        if (!load_page_to_memory(p, page_num)) {
            p->has_memory_error = true;
            stringstream ss;
            ss << "0x" << hex << addr;
            p->error_message = ss.str();
            p->error_time = chrono::system_clock::now();
            return;
        }
        
        auto &page = p->page_table->pages[page_num];
        int frame_idx = page.frame_number;
        
        if (frame_idx >= 0 && frame_idx < (int)physical_memory.size()) {
            lock_guard<mutex> lock(memory_mutex);
            if (offset < physical_memory[frame_idx].data.size()) {
                physical_memory[frame_idx].data[offset] = value;
                page.dirty = true;
            }
        }
    }

    
    void exec_inst(shared_ptr<Process> p, int cid) {
        lock_guard<mutex> lk(p->m);
        
        // check for memory error
        if (p->has_memory_error) {
            p->state = P_ERROR;
            return;
        }
        
        if (p->state == P_FINISHED || p->ip >= p->instructions.size()) {
            p->state = P_FINISHED;
            return;
        }

        auto &in = p->instructions[p->ip];

        if (in.opcode == "PRINT") {
            string msg_template = in.args[0];
            auto now = chrono::system_clock::now();
            time_t t = chrono::system_clock::to_time_t(now);
            struct tm tm;
        #ifdef _WIN32
            localtime_s(&tm, &t);
        #else
            localtime_r(&t, &tm);
        #endif
            char b[64];
            strftime(b, sizeof(b), "(%m/%d/%Y %I:%M:%S%p)", &tm);
            ostringstream msg;
            msg << b << " Core:" << cid << " \"" << msg_template << "\"";
            p->logs.push_back(msg.str());
            p->ip++;
        }
        else if (in.opcode == "ADD") {
            auto get = [&](const string &s)->uint32_t {
                if (!s.empty() && isdigit(static_cast<unsigned char>(s[0]))) return (uint32_t)stoi(s);
                if (!p->vars.count(s)) p->vars[s] = 0;
                return p->vars[s];
            };
            if (p->vars.size() < 32) {
                int64_t r = (int64_t)get(in.args[1]) + (int64_t)get(in.args[2]);
                p->vars[in.args[0]] = clamp_uint16(r);
            }
            p->ip++;
        }
        else if (in.opcode == "SUBTRACT") {
            auto get = [&](const string &s)->uint32_t {
                if (!s.empty() && isdigit(static_cast<unsigned char>(s[0]))) return (uint32_t)stoi(s);
                if (!p->vars.count(s)) p->vars[s] = 0;
                return p->vars[s];
            };
            if (p->vars.size() < 32) {
                int64_t r = (int64_t)get(in.args[1]) - (int64_t)get(in.args[2]);
                p->vars[in.args[0]] = clamp_uint16(r);
            }
            p->ip++;
        }
        else if (in.opcode == "DECLARE") {
            if (p->vars.size() < 32) {
                string var = in.args.size() > 0 ? in.args[0] : "";
                int64_t val = 0;
                if (in.args.size() > 1) {
                    try { val = stoll(in.args[1]); } catch(...) { val = 0; }
                }
                if (!var.empty()) p->vars[var] = clamp_uint16(val);
            }
            p->ip++;
        }
        else if (in.opcode == "SLEEP") {
            uint64_t ticks = 0;
            if (in.args.size() > 0) {
                try { ticks = stoull(in.args[0]); } catch(...) { ticks = 0; }
            }
            p->ip++;
            {
                lock_guard<mutex> tl(tick_mutex);
                p->wake_tick = cpu_tick + max<uint64_t>(1, ticks);
            }
            p->state = P_SLEEPING;
        }
        
        else if (in.opcode == "READ") {
            if (in.args.size() >= 2) {
                string var = in.args[0];
                string addr_str = in.args[1];
                if (addr_str.find("0x") == 0 || addr_str.find("0X") == 0) {
                    addr_str = addr_str.substr(2);
                }
                uint64_t addr = hex_to_uint64(addr_str);
                uint16_t value = read_memory(p, addr);
                
                if (!p->has_memory_error && p->vars.size() < 32) {
                    p->vars[var] = value;
                }
            }
            p->ip++;
        }
        
        else if (in.opcode == "WRITE") {
            if (in.args.size() >= 2) {
                string addr_str = in.args[0];
                if (addr_str.find("0x") == 0 || addr_str.find("0X") == 0) {
                    addr_str = addr_str.substr(2);
                }
                uint64_t addr = hex_to_uint64(addr_str);
                
                uint16_t value = 0;
                if (!in.args[1].empty() && isdigit(static_cast<unsigned char>(in.args[1][0]))) {
                    value = (uint16_t)stoi(in.args[1]);
                } else if (p->vars.count(in.args[1])) {
                    value = p->vars[in.args[1]];
                }
                
                write_memory(p, addr, value);
            }
            p->ip++;
        }
        else if (in.opcode == "FOR") {
            uint32_t repeats = 1;
            if (in.args.size() > 0) {
                try { repeats = (uint32_t)stoul(in.args[0]); } catch(...) { repeats = 1; }
            }
            string enc_inner = in.args.size() > 1 ? in.args[1] : "";
            vector<Instr> inner = decode_inner_instructions(enc_inner);
            if (inner.empty() || repeats == 0) {
                p->ip++;
            } else {
                vector<Instr> expanded;
                expanded.reserve(inner.size() * (size_t)repeats);
                for (uint32_t r = 0; r < repeats; ++r) {
                    for (auto &ii : inner) expanded.push_back(ii);
                }
                auto it_begin = p->instructions.begin();
                p->instructions.erase(it_begin + p->ip);
                p->instructions.insert(it_begin + p->ip, expanded.begin(), expanded.end());
            }
        }
        else {
            p->ip++;
        }

        if (config.delays_per_exec > 0)
            this_thread::sleep_for(std::chrono::milliseconds(config.delays_per_exec));

        if (p->ip >= p->instructions.size() && p->state != P_SLEEPING) p->state = P_FINISHED;
    }

   
    shared_ptr<Process> create_process(const string &name, uint64_t mem_size = 0, 
                                       const vector<Instr> &custom_instrs = {}) {
        auto p = make_shared<Process>();
        {
            lock_guard<mutex> lk(processes_mutex);
            p->pid = next_pid++;
            p->name = name;
        }

        // set up memory if provided
        if (mem_size > 0) {
            p->memory_size = mem_size;
            p->page_table = new PageTable(mem_size, config.mem_per_frame);
        }

        p->vars["x"] = 0;

        if (custom_instrs.empty()) {
            p->instructions = random_instructions_for(p->name);
        } else {
            p->instructions = custom_instrs;
        }
        
        p->start_time = chrono::system_clock::now();
        
        {
            lock_guard<mutex> lk(processes_mutex);
            processes_by_pid[p->pid] = p;
            processes_by_name[p->name] = p;
            total_processes_created++;
        }

        {
            lock_guard<mutex> lk(ready_mutex);
            ready_queue.push_back(p->pid);
        }
        ready_cv.notify_one();
        return p;
    }

    
    void cpu_worker(int cid) {
        while (!stop_all) {
            int pid = -1;
            {
                unique_lock<mutex> lk(ready_mutex);
                if (ready_queue.empty()) {
                    lk.unlock();
                    this_thread::sleep_for(chrono::milliseconds(1));
                    continue;
                }
                pid = ready_queue.front();
                ready_queue.pop_front();
            }

            shared_ptr<Process> p;
            {
                lock_guard<mutex> lk(processes_mutex);
                if (!processes_by_pid.count(pid)) continue;
                p = processes_by_pid[pid];
            }

            {
                lock_guard<mutex> lk(p->m);
                if (p->state == P_FINISHED || p->state == P_SLEEPING || p->state == P_ERROR) continue;
                p->state = P_RUNNING;
                p->current_core = cid;
            }

            if (cid >= 0 && cid < (int)core_busy.size()) {
                if (core_busy[cid] == 0) {
                    core_busy[cid] = 1;
                    busy_core_count.fetch_add(1, std::memory_order_relaxed);
                }
            }

            total_busy_ticks.fetch_add(1, std::memory_order_relaxed);

            if (config.scheduler == "rr") {
                for (uint64_t i = 0; i < config.quantum_cycles; i++) {
                    exec_inst(p, cid);
                    total_executed_instructions++;
                    if (p->state != P_RUNNING) break;
                }
            } else {
                exec_inst(p, cid);
                total_executed_instructions++;
            }

            {
                lock_guard<mutex> lk(p->m);
                if (p->state == P_RUNNING) { 
                    p->state = P_READY;
                    p->current_core = -1;
                }
            }

            {
                lock_guard<mutex> lk(ready_mutex);
                if (p->state == P_READY) ready_queue.push_back(pid);
            }

            if (cid >= 0 && cid < (int)core_busy.size()) {
                if (core_busy[cid] == 1) {
                    core_busy[cid] = 0;
                    busy_core_count.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
    }

    
    void scheduler_loop() {
        while (!stop_all) {
            {
                lock_guard<mutex> lk(tick_mutex);
                cpu_tick++;
            }

            total_ticks_elapsed.fetch_add(1, std::memory_order_relaxed);

            // wake sleeping processes
            {
                lock_guard<mutex> lk(processes_mutex);
                vector<int> to_ready;
                for (auto &kv : processes_by_pid) {
                    auto p = kv.second;
                    lock_guard<mutex> pl(p->m);
                    if (p->state == P_SLEEPING && p->wake_tick <= cpu_tick) {
                        p->state = P_READY;
                        to_ready.push_back(p->pid);
                    }
                }
                if (!to_ready.empty()) {
                    lock_guard<mutex> rlk(ready_mutex);
                    for (int pid : to_ready) ready_queue.push_back(pid);
                    ready_cv.notify_all();
                }
            }

            // auto-generate processes (w/ memory)
            if (generator_running && config.batch_process_freq > 0) {
                bool should_create = false;
                {
                    lock_guard<mutex> lk(tick_mutex);
                    if (config.batch_process_freq > 0 && (cpu_tick % config.batch_process_freq == 0)) 
                        should_create = true;
                }
                if (should_create) {
                    int pid;
                    {
                        lock_guard<mutex> lk(processes_mutex);
                        pid = next_pid;
                    }
                    // auto-generated processes get memory
                    uniform_int_distribution<uint64_t> mem_dist(
                        config.min_mem_per_proc, config.max_mem_per_proc
                    );
                    uint64_t mem_size = mem_dist(rng);
                    uint64_t rounded = 64;
                    while (rounded < mem_size && rounded < 65536) rounded *= 2;
                    create_process("auto-process" + to_string(pid), rounded);
                }
            }

            this_thread::sleep_for(10ms);
        }
    }

    MemoryStats get_memory_stats() {
        MemoryStats stats;
        lock_guard<mutex> lock(memory_mutex);
        
        stats.total_memory = config.max_overall_mem;
        stats.total_frames = physical_memory.size();
        stats.used_frames = 0;
        
        for (const auto &frame : physical_memory) {
            if (frame.occupied) stats.used_frames++;
        }
        
        stats.free_frames = stats.total_frames - stats.used_frames;
        stats.used_memory = stats.used_frames * config.mem_per_frame;
        stats.free_memory = stats.free_frames * config.mem_per_frame;
        stats.num_pages_in = num_pages_paged_in.load();
        stats.num_pages_out = num_pages_paged_out.load();
        
        return stats;
    }

    
    string screen_get_attached_output(const string &process_name) {
        lock_guard<mutex> lock(processes_mutex);
        if (!processes_by_name.count(process_name))
            return "Process " + process_name + " not found.\n";

        auto p = processes_by_name[process_name];
        lock_guard<mutex> pl(p->m);

        auto logs = p->logs;

        stringstream ss;
        ss << "Process name: " << p->name << "\n";
        ss << "ID: " << p->pid << "\n";
        
        // show memory if allocated
        if (p->memory_size > 0) {
            ss << "Memory: " << p->memory_size << " bytes\n";
        }
        
        ss << "Logs (Latest 10):\n";
        size_t start_index = (p->logs.size() > 10) ? p->logs.size() - 10 : 0;
        for (size_t i = start_index; i < p->logs.size(); ++i) {
            ss << p->logs[i] << "\n";
        }
        
        ss << "\nCurrent instruction line: " << p->ip << "\n";
        ss << "Lines of code: " << p->instructions.size() << "\n";
        ss << "Variables:\n";
        
        for (auto &kv : p->vars) ss << kv.first << " = " << kv.second << "\n";
        
        if (p->state == P_SLEEPING) {
            ss << "State: SLEEPING (wakes at tick " << p->wake_tick << ")\n";
        } else if (p->state == P_RUNNING) {
            ss << "State: RUNNING\n";
        } else if (p->state == P_READY) {
            ss << "State: READY\n";
        } else if (p->state == P_ERROR) {
            // show memory error
            time_t t = chrono::system_clock::to_time_t(p->error_time);
            struct tm tm;
            #ifdef _WIN32
                localtime_s(&tm, &t);
            #else
                localtime_r(&t, &tm);
            #endif
            char b[16];
            strftime(b, sizeof(b), "%H:%M:%S", &tm);
            ss << "State: ERROR - Memory access violation at " << b 
               << ". " << p->error_message << " invalid.\n";
        } else if (p->state == P_FINISHED) {
            ss << "\n\nFinished!\n";
        }
        return ss.str();
    }

    
    bool read_config_file(const string &fn) {
        ifstream f(fn);
        if (!f.good()) return false;
        string l;
        while (getline(f, l)) {
            l = trim(l);
            if (l.empty()) continue;
            auto p = split_space(l);
            if (p.size() < 2) continue;
            string k = p[0];
            string v = p[1];
            
            // Remove quotes
            if (!v.empty() && v.front() == '"' && v.back() == '"') {
                v = v.substr(1, v.length() - 2);
            }
            
            try {
                if (k == "num-cpu") config.num_cpu = max(1, stoi(v));
                else if (k == "scheduler") config.scheduler = v;
                else if (k == "quantum-cycles") config.quantum_cycles = max(1ULL, stoull(v));
                else if (k == "batch-process-freq") config.batch_process_freq = max(1ULL, stoull(v));
                else if (k == "min-ins") config.min_ins = stoi(v);
                else if (k == "max-ins") config.max_ins = stoi(v);
                else if (k == "delays-per-exec") config.delays_per_exec = stoull(v);
                else if (k == "max-overall-mem") config.max_overall_mem = stoull(v);
                else if (k == "mem-per-frame") config.mem_per_frame = stoull(v);
                else if (k == "min-mem-per-proc") config.min_mem_per_proc = stoull(v);
                else if (k == "max-mem-per-proc") config.max_mem_per_proc = stoull(v);
            } catch (...) {}
        }
        return true;
    }

    
    bool handle_command(const string &cmd) {
        string c = trim(cmd);

        if (attached_process.empty() && !(c.rfind("screen -s ",0)==0) && 
            !(c.rfind("screen -c ",0)==0) && c != "process-smi" && c != "exit")
            last_screen_s_process.clear();

        if (c == "initialize") {
            if (initialized) { setMessage("Already initialized\n"); return true; }
            
            if (!read_config_file("config.txt")) {
                setMessage("Error: config.txt not found\n");
                return true;
            }
            
            // MCO2: Validate memory parameters
            if (!is_valid_memory_size(config.max_overall_mem) || 
                !is_valid_memory_size(config.mem_per_frame) ||
                !is_valid_memory_size(config.min_mem_per_proc) ||
                !is_valid_memory_size(config.max_mem_per_proc)) {
                setMessage("Error: Invalid memory parameters in config.txt\n");
                return true;
            }

            
            init_memory_manager();
            
            initialized = true;
            stop_all = false;
            core_busy.assign(config.num_cpu, 0);
            busy_core_count.store(0);
            total_busy_ticks.store(0);
            total_ticks_elapsed.store(0);

            if (!scheduler_thread.joinable()) scheduler_thread = thread(scheduler_loop);
            for (uint32_t i = 0; i < config.num_cpu; ++i)
                cpu_workers.emplace_back(cpu_worker, (int)i);

            setMessage("Process scheduler initialized\n");
            return true;
        }
        
        if (!initialized && c != "initialize") {
            setMessage("Please run 'initialize' first\n");
            return true;
        }
        
        if (c == "scheduler-start") {
            generator_running = true; setMessage("Scheduler started\n"); return true;
        }
        if (c == "scheduler-stop") {
            generator_running = false; setMessage("Scheduler stopped\n"); return true;
        }
        
        if (c == "screen -ls") {
            stringstream ss;
            uint64_t elapsed = total_ticks_elapsed.load();
            uint64_t busy_t = total_busy_ticks.load();
            double util = 0.0;
            if (elapsed > 0 && config.num_cpu > 0)
                util = 100.0 * ((double)busy_t / (double)(elapsed * config.num_cpu));
            if (util > 100.0) util = 100.0;

            double avg_busy = (elapsed > 0) ? ((double)busy_t / (double)elapsed) : 0.0;
            if (avg_busy > config.num_cpu) avg_busy = config.num_cpu;

            ss << "=== CPU UTILIZATION REPORT ===\n";
            ss << "CPU utilization: " << fixed << setprecision(2) << util << "%\n";
            ss << "Cores used: " << (int)round(avg_busy) << "\n";
            ss << "Cores available: " << max(0, (int)config.num_cpu - (int)round(avg_busy)) << "\n\n";

            vector<shared_ptr<Process>> running;
            vector<shared_ptr<Process>> finished;

            {
                lock_guard<mutex> lk(processes_mutex);
                for (auto &kv : processes_by_pid) {
                    auto p = kv.second;
                    lock_guard<mutex> pl(p->m);
                    if (p->state != P_FINISHED && p->state != P_ERROR)
                        running.push_back(p);
                    else
                        finished.push_back(p);
                }
            }

            sort(running.begin(), running.end(), [](auto &a, auto &b){ return a->pid < b->pid; });
            sort(finished.begin(), finished.end(), [](auto &a, auto &b){ return a->pid < b->pid; });

            if (running.size() > 10) running.erase(running.begin(), running.end() - 10);
            if (finished.size() > 10) finished.erase(finished.begin(), finished.end() - 10);

            ss << "Running processes (latest 10):\n";
            for (auto &p : running) {
                string core_display = (p->current_core >=0) ? to_string(p->current_core) : "-";
                string ts = format_time(p->start_time);
                ss << p->name << "  " << ts << "  Core: " << core_display
                << "  " << p->ip << " / " << p->instructions.size() << "\n";
            }

            ss << "\nFinished processes (latest 10):\n";
            for (auto &p : finished) {
                string ts = format_time(p->start_time);
                ss << p->name << "  " << ts << "  Finished  "
                << p->instructions.size() << " / " << p->instructions.size() << "\n";
            }

            total_busy_ticks.store(0);
            total_ticks_elapsed.store(0);

            setMessage(ss.str());
            return true;
        }
        
        // MCO2: screen -s <process_name> <process_memory_size>
        if (c.rfind("screen -s ", 0) == 0) {
            auto parts = split_space(c);
            if (parts.size() < 4) {
                setMessage("Usage: screen -s <process_name> <process_memory_size>\n");
                return true;
            }

            string name = parts[2];
            uint64_t mem_size = 0;
            try {
                mem_size = stoull(parts[3]);
            } catch (...) {
                setMessage("Invalid memory size\n");
                return true;
            }

            if (!is_valid_memory_size(mem_size)) {
                setMessage("Invalid memory allocation. Must be power of 2 between 64 and 65536\n");
                return true;
            }

            bool exists = false;
            {
                lock_guard<mutex> lk(processes_mutex);
                exists = processes_by_name.count(name) > 0;
            }

            if (exists) {
                setMessage("Process already exists\n");
                return true;
            }

            create_process(name, mem_size);
            attached_process = name;
            last_screen_s_process = name;

            setMessage(screen_get_attached_output(name));
            return true;
        }
        
        
        if (c.rfind("screen -c ", 0) == 0) {
            size_t first_quote = c.find('"');
            size_t last_quote = c.rfind('"');
            
            if (first_quote == string::npos || last_quote == string::npos || first_quote == last_quote) {
                setMessage("Usage: screen -c <process_name> <process_memory_size> \"<instructions>\"\n");
                return true;
            }

            string before_quote = c.substr(0, first_quote);
            auto parts = split_space(before_quote);
            
            if (parts.size() < 4) {
                setMessage("Usage: screen -c <process_name> <process_memory_size> \"<instructions>\"\n");
                return true;
            }

            string name = parts[2];
            uint64_t mem_size = 0;
            try {
                mem_size = stoull(parts[3]);
            } catch (...) {
                setMessage("Invalid memory size\n");
                return true;
            }

            if (!is_valid_memory_size(mem_size)) {
                setMessage("Invalid memory allocation\n");
                return true;
            }

            string instr_str = c.substr(first_quote + 1, last_quote - first_quote - 1);
            auto instrs = parse_user_instructions(instr_str);

            if (instrs.empty() || instrs.size() > 50) {
                setMessage("Invalid command: instruction count must be 1-50\n");
                return true;
            }

            bool exists = false;
            {
                lock_guard<mutex> lk(processes_mutex);
                exists = processes_by_name.count(name) > 0;
            }

            if (exists) {
                setMessage("Process already exists\n");
                return true;
            }

            create_process(name, mem_size, instrs);
            attached_process = name;
            last_screen_s_process = name;

            setMessage(screen_get_attached_output(name));
            return true;
        }
        
        if (c.rfind("screen -r ", 0) == 0) {
            string name = trim(c.substr(10));
            shared_ptr<Process> p;

            {
                lock_guard<mutex> lk(processes_mutex);
                if (!processes_by_name.count(name)) {
                    setMessage("Process " + name + " not found\n");
                    return true;
                }
                p = processes_by_name[name];
            }

            
            if (p->has_memory_error) {
                time_t t = chrono::system_clock::to_time_t(p->error_time);
                struct tm tm;
                #ifdef _WIN32
                    localtime_s(&tm, &t);
                #else
                    localtime_r(&t, &tm);
                #endif
                char b[16];
                strftime(b, sizeof(b), "%H:%M:%S", &tm);
                setMessage("Process " + name + " shut down due to memory access violation error that occurred at " + 
                          string(b) + ". " + p->error_message + " invalid.\n");
                return true;
            }

            attached_process = name;
            setMessage(screen_get_attached_output(name));
            return true;
        }
        
        
        if (c == "process-smi") {
            auto stats = get_memory_stats();
            stringstream ss;
            
            ss << "=========================================\n";
            ss << "| PROCESS-SMI V1.0      Driver Version |\n";
            ss << "=========================================\n\n";
            ss << "Memory Usage: " << stats.used_memory << " / " << stats.total_memory << " bytes\n";
            ss << "Memory Utilization: " << fixed << setprecision(1) 
               << (100.0 * stats.used_memory / stats.total_memory) << "%\n\n";
            
            ss << "=========================================\n";
            ss << "Running Processes and Memory Usage:\n";
            ss << "=========================================\n";
            
            vector<shared_ptr<Process>> procs;
            {
                lock_guard<mutex> lk(processes_mutex);
                for (auto &kv : processes_by_pid) {
                    procs.push_back(kv.second);
                }
            }
            
            for (auto &p : procs) {
                lock_guard<mutex> pl(p->m);
                if (p->state != P_FINISHED && p->state != P_ERROR && p->memory_size > 0) {
                    ss << p->name << " (PID " << p->pid << "): " 
                       << p->memory_size << " bytes\n";
                }
            }
            
            setMessage(ss.str());
            return true;
        }

        
        if (c == "vmstat") {
            auto stats = get_memory_stats();
            uint64_t total_ticks = total_ticks_elapsed.load();
            uint64_t busy_ticks = total_busy_ticks.load();
            uint64_t idle_ticks = total_ticks * config.num_cpu - busy_ticks;
            
            stringstream ss;
            ss << "=========================================\n";
            ss << "VIRTUAL MEMORY STATISTICS\n";
            ss << "=========================================\n\n";
            ss << "Total memory:     " << stats.total_memory << " bytes\n";
            ss << "Used memory:      " << stats.used_memory << " bytes\n";
            ss << "Free memory:      " << stats.free_memory << " bytes\n\n";
            ss << "Idle CPU ticks:   " << idle_ticks << "\n";
            ss << "Active CPU ticks: " << busy_ticks << "\n";
            ss << "Total CPU ticks:  " << (total_ticks * config.num_cpu) << "\n\n";
            ss << "Num paged in:     " << stats.num_pages_in << "\n";
            ss << "Num paged out:    " << stats.num_pages_out << "\n";
            
            setMessage(ss.str());
            return true;
        }

        if (c == "report-util") {
            stringstream ss;
            uint64_t elapsed = total_ticks_elapsed.load();
            uint64_t busy_t = total_busy_ticks.load();
            double util = 0.0;
            if (elapsed > 0 && config.num_cpu > 0)
                util = 100.0 * ((double)busy_t / (double)(elapsed * config.num_cpu));
            if (util > 100.0) util = 100.0;

            double avg_busy = (elapsed > 0) ? ((double)busy_t / (double)elapsed) : 0.0;
            if (avg_busy > config.num_cpu) avg_busy = config.num_cpu;

            ss << "=== CPU UTILIZATION REPORT ===\n";
            ss << "CPU utilization: " << fixed << setprecision(2) << util << "%\n";
            ss << "Cores used: " << (int)round(avg_busy) << "\n";
            ss << "Cores available: " << max(0, (int)config.num_cpu - (int)round(avg_busy)) << "\n\n";

            ss << "Running processes:\n";
            vector<shared_ptr<Process>> finished;
            {
                lock_guard<mutex> lk(processes_mutex);
                for (auto &kv : processes_by_pid) {
                    auto p = kv.second;
                    lock_guard<mutex> pl(p->m);
                    if (p->state != P_FINISHED && p->state != P_ERROR) {
                        string core_display = (p->current_core >=0) ? to_string(p->current_core) : "-";
                        string ts = format_time(p->start_time);
                        ss << p->name << "  " << ts << "  Core: " << core_display
                        << "  " << p->ip << " / " << p->instructions.size() << "\n";
                    }
                        else finished.push_back(p);
                }
            }

            ss << "\nFinished processes:\n";
            for (auto &p : finished)
                ss << p->name << "  Finished " << p->instructions.size() << " / " << p->instructions.size() << "\n";

            ofstream logFile("csopesy-log.txt");
            logFile << ss.str();
            logFile.close();

            setMessage("Report written to csopesy-log.txt\n");
            return true;
        }
        
        if (c == "exit") {
            if (!attached_process.empty()) {
                attached_process.clear();
                last_screen_s_process.clear();
                return true;
            }
            return false;
        }

        return false;
    }

    void shutdown_module() {
        stop_all = true;
        ready_cv.notify_all();
        if (scheduler_thread.joinable()) scheduler_thread.join();
        for (auto &t : cpu_workers) if (t.joinable()) t.join();
        cpu_workers.clear();
        initialized = false;
    }

    void init_module() {
        initialized = false;
        generator_running = false;
        stop_all = false;
        core_busy.clear();
        busy_core_count.store(0);
        total_busy_ticks.store(0);
        total_ticks_elapsed.store(0);
    }

} // namespace CSOPESY