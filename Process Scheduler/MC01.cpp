#include "MC01.h"

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

extern void setMessage(const std::string &s);
using namespace std;

namespace MC01 {

    // default setup
    struct Config {
        uint32_t num_cpu = 4;
        string scheduler = "rr";
        uint64_t quantum_cycles = 5;
        uint64_t batch_process_freq = 1;
        uint32_t min_ins = 1000;
        uint32_t max_ins = 2000;
        uint64_t delays_per_exec = 0;
    } config;

    // module state
    atomic<bool> initialized(false);
    atomic<bool> generator_running(false);
    atomic<bool> stop_all(false);

    enum ProcState { P_READY, P_RUNNING, P_SLEEPING, P_FINISHED };

    struct Instr {
        string opcode;
        vector<string> args;
    };

    struct Process {
        int pid;
        string name;
        vector<Instr> instructions;
        size_t ip = 0;
        map<string,uint32_t> vars;
        vector<string> logs;
        ProcState state = P_READY;
        uint64_t wake_tick = 0;
        mutex m;
        chrono::system_clock::time_point start_time;
        int current_core = -1; // Which core it's running on (-1 if not running)
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

    // core busy flags
    vector<char> core_busy;
    atomic<uint64_t> busy_core_count(0);

    // utilization tracking
    atomic<uint64_t> total_busy_ticks(0);
    atomic<uint64_t> total_ticks_elapsed(0);

    // helper: trimming and splitting
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

    // Instruction constructors
    static Instr make_PRINT(const string &msg){ Instr i; i.opcode="PRINT"; i.args={msg}; return i; }
    static Instr make_ADD(const string &d,const string &a,const string &b){ Instr i; i.opcode="ADD"; i.args={d,a,b}; return i; }
    static Instr make_SUB(const string &d,const string &a,const string &b){ Instr i; i.opcode="SUBTRACT"; i.args={d,a,b}; return i; }
    static Instr make_DECLARE(const string &v,const string &val){ Instr i; i.opcode="DECLARE"; i.args={v,val}; return i; }
    static Instr make_SLEEP(const string &ticks){ Instr i; i.opcode="SLEEP"; i.args={ticks}; return i; }
    // FOR will be encoded as: args[0] = repeats, args[1] = encoded inner instructions string
    // inner encoding: each instr separated by ';', each instr fields separated by ':'
    // examples: "PRINT:Hello world;ADD:x:x:1"
    static Instr make_FOR(uint32_t repeats, const string &encoded_inner){
        Instr i; i.opcode="FOR"; i.args={to_string(repeats), encoded_inner}; return i;
    }

    // parse encoded instruction token like "PRINT:Hello world" or "ADD:x:x:1"
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
            // everything after first ':' is considered part of the message;
            // reconstruct message substring after "PRINT:"
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
        } else {
            // fallback: unknown -> treat as PRINT full
            out.opcode = "PRINT";
            out.args = {enc};
        }
        return out;
    }

    // helper to decode inner encoded string into vector<Instr>
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

    // Question 4 Scenario
    static vector<Instr> make_test_instructions_q4() {
        vector<Instr> out;
        uniform_int_distribution<int> add_val(1, 10);
        // 50,000 pairs = 100,000 instructions
        for (int i = 0; i < 50000; ++i) {
            // PRINT the VALUE of x (not a static string)
            out.push_back(make_PRINT("Value from: x")); // We'll interpret "x" specially
            // ADD
            out.push_back(make_ADD("x", "x", to_string(add_val(rng))));
        }
        return out;
    }

    // Random instruction generation with support for FOR, DECLARE, SUBTRACT, SLEEP
    static vector<Instr> random_instructions_for(const string &pname) {
        uniform_int_distribution<int> d(config.min_ins, config.max_ins);
        int n = d(rng);
        if (n < 2) n = 2;
        vector<Instr> out;
        uniform_int_distribution<int> op_choice(0, 6); // choose among several opcodes
        uniform_int_distribution<int> smallval(1,10);
        uniform_int_distribution<int> sleepval(1,5);
        uniform_int_distribution<int> for_inner_len(1,4);
        uniform_int_distribution<int> for_repeats(1,4);
        for (int i = 0; i < n; ++i) {
            int choice = op_choice(rng);
            if (choice == 0) {
                out.push_back(make_PRINT("Hello world from " + pname + "!"));
            } else if (choice == 1) {
                out.push_back(make_ADD("x", "x", to_string(smallval(rng))));
            } else if (choice == 2) {
                out.push_back(make_SUB("x", "x", to_string(smallval(rng))));
            } else if (choice == 3) {
                // DECLARE new var with initial small value
                string var = string("v") + to_string(smallval(rng));
                out.push_back(make_DECLARE(var, to_string(smallval(rng))));
            } else if (choice == 4) {
                // SLEEP for a few ticks
                out.push_back(make_SLEEP(to_string((int)sleepval(rng))));
            } else if (choice == 5) {
                // FOR with small inner sequence (depth up to 2 when inner FORs randomly included)
                int inner_count = for_inner_len(rng);
                vector<string> inner_enc;
                for (int j=0;j<inner_count;j++) {
                    int ic = op_choice(rng) % 4; // limited set for inner
                    if (ic == 0) inner_enc.push_back("PRINT:Hello from FOR");
                    else if (ic == 1) inner_enc.push_back("ADD:x:x:" + to_string(smallval(rng)));
                    else if (ic == 2) inner_enc.push_back("SUBTRACT:x:x:" + to_string(smallval(rng)));
                    else inner_enc.push_back("SLEEP:" + to_string((int)sleepval(rng)));
                }
                // join inner_enc with ';'
                string enc;
                for (size_t k=0;k<inner_enc.size();k++){
                    if (k) enc += ";";
                    enc += inner_enc[k];
                }
                int repeats = for_repeats(rng);
                out.push_back(make_FOR((uint32_t)repeats, enc));
            } else {
                // fallback PRINT
                out.push_back(make_PRINT("Hello world from " + pname + "!"));
            }
        }
        return out;
    }
    
    shared_ptr<Process> create_process(const string &name) {
        auto p = make_shared<Process>();
        {
            lock_guard<mutex> lk(processes_mutex);
            p->pid = next_pid++;
            p->name = name;
        }

        p->vars["x"] = 0;

        // Special Case for that special Scenario 4
        if (name == "proc-01") {
            p->instructions = make_test_instructions_q4();
        } else {
            p->instructions = random_instructions_for(p->name);
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

    // Execute a single instruction for process p on core cid
    void exec_inst(shared_ptr<Process> p,int cid) {
        lock_guard<mutex> lk(p->m);
        if (p->state == P_FINISHED || p->ip >= p->instructions.size()) {
            p->state = P_FINISHED;
            return;
        }

        auto &in = p->instructions[p->ip];

        if (in.opcode == "PRINT") {
            string msg_template = in.args[0];
            string final_msg = msg_template;

            // Special handling for "Value from: x"
            if (msg_template == "Value from: x") {
                uint32_t x_val = 0;
                if (p->vars.count("x")) {
                    x_val = p->vars["x"];
                }
                final_msg = "Value from: " + to_string(x_val);
            }

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
            msg << b << " Core:" << cid << " \"" << final_msg << "\"";
            p->logs.push_back(msg.str());
            p->ip++;
        }
        else if (in.opcode == "ADD") {
            auto get = [&](const string &s)->uint32_t {
                if (!s.empty() && isdigit(static_cast<unsigned char>(s[0]))) return (uint32_t)stoi(s);
                if (!p->vars.count(s)) p->vars[s] = 0;
                return p->vars[s];
            };
            int64_t r = (int64_t)get(in.args[1]) + (int64_t)get(in.args[2]);
            p->vars[in.args[0]] = clamp_uint16(r);
            p->ip++;
        }
        else if (in.opcode == "SUBTRACT") {
            auto get = [&](const string &s)->uint32_t {
                if (!s.empty() && isdigit(static_cast<unsigned char>(s[0]))) return (uint32_t)stoi(s);
                if (!p->vars.count(s)) p->vars[s] = 0;
                return p->vars[s];
            };
            int64_t r = (int64_t)get(in.args[1]) - (int64_t)get(in.args[2]);
            p->vars[in.args[0]] = clamp_uint16(r);
            p->ip++;
        }
        else if (in.opcode == "DECLARE") {
            string var = in.args.size() > 0 ? in.args[0] : "";
            int64_t val = 0;
            if (in.args.size() > 1) {
                try { val = stoll(in.args[1]); } catch(...) { val = 0; }
            }
            if (!var.empty()) p->vars[var] = clamp_uint16(val);
            p->ip++;
        }
        else if (in.opcode == "SLEEP") {
            uint64_t ticks = 0;
            if (in.args.size() > 0) {
                try { ticks = stoull(in.args[0]); } catch(...) { ticks = 0; }
            }
            // advance instruction pointer, set sleeping state and wake tick
            p->ip++;
            {
                lock_guard<mutex> tl(tick_mutex);
                p->wake_tick = cpu_tick + max<uint64_t>(1, ticks);
            }
            p->state = P_SLEEPING;
            // Do not progress further; scheduler will skip this process until wake_tick reached
        }
        else if (in.opcode == "FOR") {
            // Expand the FOR instruction in-place: decode inner, repeat, then replace the FOR at ip
            uint32_t repeats = 1;
            if (in.args.size() > 0) {
                try { repeats = (uint32_t)stoul(in.args[0]); } catch(...) { repeats = 1; }
            }
            string enc_inner = in.args.size() > 1 ? in.args[1] : "";
            vector<Instr> inner = decode_inner_instructions(enc_inner);
            if (inner.empty() || repeats == 0) {
                // nothing to do, just advance
                p->ip++;
            } else {
                // build expanded sequence
                vector<Instr> expanded;
                expanded.reserve(inner.size() * (size_t)repeats);
                for (uint32_t r = 0; r < repeats; ++r) {
                    for (auto &ii : inner) expanded.push_back(ii);
                }
                // replace FOR with expanded sequence
                // erase the FOR at p->ip and insert expanded at same position
                auto it_begin = p->instructions.begin();
                p->instructions.erase(it_begin + p->ip);
                p->instructions.insert(it_begin + p->ip, expanded.begin(), expanded.end());
                // do not advance ip so that the next exec_inst will execute the first expanded instruction
            }
        }
        else {
            // unknown opcode: skip
            p->ip++;
        }

        if (config.delays_per_exec > 0)
            this_thread::sleep_for(std::chrono::milliseconds(config.delays_per_exec));

        if (p->ip >= p->instructions.size() && p->state != P_SLEEPING) p->state = P_FINISHED;
    }

    void cpu_worker(int cid) {
        while (!stop_all) {
            int pid = -1;
            {
                unique_lock<mutex> lk(ready_mutex);
                if (ready_queue.empty()) {
                    // wait a short time; also allow thread to be responsive to stop_all
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
                if (p->state == P_FINISHED || p->state == P_SLEEPING) continue;
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

    // scheduler_loop: advance cpu_tick, auto-generate processes, and wake sleeping processes
    void scheduler_loop() {
        while (!stop_all) {
            {
                lock_guard<mutex> lk(tick_mutex);
                cpu_tick++;
            }

            total_ticks_elapsed.fetch_add(1, std::memory_order_relaxed);

            // wake sleeping processes whose wake_tick <= cpu_tick
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

            if (generator_running && config.batch_process_freq > 0) {
                // create process every batch_process_freq ticks
                bool should_create = false;
                {
                    lock_guard<mutex> lk(tick_mutex);
                    if (config.batch_process_freq > 0 && (cpu_tick % config.batch_process_freq == 0)) should_create = true;
                }
                if (should_create) {
                    int pid;
                    {
                        lock_guard<mutex> lk(processes_mutex);
                        pid = next_pid;
                    }
                    create_process("auto-process" + to_string(pid));
                }
            }

            this_thread::sleep_for(10ms);
        }
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
            try {
                if (k == "num-cpu") config.num_cpu = max(1, stoi(v));
                else if (k == "scheduler") config.scheduler = v;
                else if (k == "quantum-cycles") config.quantum_cycles = max(1ULL, stoull(v));
                else if (k == "batch-process-freq") config.batch_process_freq = max(1ULL, stoull(v));
                else if (k == "min-ins") config.min_ins = stoi(v);
                else if (k == "max-ins") config.max_ins = stoi(v);
                else if (k == "delays-per-exec") config.delays_per_exec = stoull(v);
            } catch (...) {}
        }
        return true;
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
        if (p->name == "proc-01") {
            // Show FIRST 20 logs for proc-01 only (just for that question lmao)
            ss << "Logs (Earliest 20 for Testing Purposes):\n";
            size_t end_index = min(p->logs.size(), (size_t)20);
            for (size_t i = 0; i < end_index; ++i) {
                ss << p->logs[i] << "\n";
            }
        } else {
            ss << "Logs (Latest 10):\n";
            // Show LAST 10 logs for all other processes
            size_t start_index = (p->logs.size() > 10) ? p->logs.size() - 10 : 0;
            for (size_t i = start_index; i < p->logs.size(); ++i) {
                ss << p->logs[i] << "\n";
            }
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
        } else if (p->state == P_FINISHED) {
            ss << "\n\nFinished!\n";
        }
        return ss.str();
    }

    bool handle_command(const string &cmd) {
        string c = trim(cmd);

        // reset last_screen_s_process unless this is screen -s or process-smi trigger
        if (!(c.rfind("screen -s ",0)==0) && c!="process-smi") 
            last_screen_s_process.clear();

        if (c == "initialize") {
            if (initialized) { setMessage("Already initialized\n"); return true; }
            read_config_file("config.txt");

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
                    if (p->state != P_FINISHED)
                        running.push_back(p);
                    else
                        finished.push_back(p);
                }
            }

            // Sort by PID for consistent ordering
            sort(running.begin(), running.end(), [](auto &a, auto &b){ return a->pid < b->pid; });
            sort(finished.begin(), finished.end(), [](auto &a, auto &b){ return a->pid < b->pid; });

            // Keep only latest 10 processes in each category
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
        if (c.rfind("screen -s ", 0) == 0) {
            string name = trim(c.substr(10));
            if (name.empty()) {
                setMessage("Invalid process name\n");
                return true;
            }

            {
                lock_guard<mutex> lk(processes_mutex);
                if (processes_by_name.count(name)) {
                    setMessage("Process already exists\n");
                    return true;
                }
            }

            create_process(name);
            attached_process = name;
            last_screen_s_process = name; 
            setMessage(screen_get_attached_output(name));
            return true;
        }
        if (c.rfind("screen -r ", 0) == 0) {
            string name = trim(c.substr(10));

            lock_guard<mutex> lk(processes_mutex);
            if (!processes_by_name.count(name)) {
                setMessage("Process " + name + " not found\n");
                return true;
            }

            attached_process = name;
            string out = screen_get_attached_output(name);
            setMessage(out);
            
            return true;
        }
        if (c == "process-smi") {
            if (last_screen_s_process.empty()) {
                setMessage("Error: process-smi is only valid immediately after screen -s\n");
                return true;
            }
            setMessage(screen_get_attached_output(last_screen_s_process));
            last_screen_s_process.clear(); 
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
                    if (p->state != P_FINISHED) {
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
            MC01::shutdown_module();
            return true;
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

}
