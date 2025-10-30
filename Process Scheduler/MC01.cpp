#include "MC01.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <map>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <iomanip>
#include <iostream>

extern void setMessage(const std::string &s);
using namespace std;

namespace MC01 {

// ---------- Config / defaults ----------
struct Config {
    uint32_t num_cpu = 1;
    string scheduler = "fcfs"; // "fcfs" or "rr"
    uint64_t quantum_cycles = 1;
    uint64_t batch_process_freq = 1;
    uint32_t min_ins = 1;
    uint32_t max_ins = 8;
    uint64_t delays_per_exec = 0;
} config;

atomic<bool> initialized(false);
atomic<bool> generator_running(false);
atomic<bool> scheduler_thread_running(false);
atomic<bool> stop_all(false);

// ---------- Process model ----------
enum ProcState { P_READY, P_RUNNING, P_SLEEPING, P_FINISHED };

struct Instr {
    string opcode; // e.g., "PRINT", "DECLARE", "ADD", "SUB", "SLEEP", "FOR_START", "FOR_END"
    vector<string> args;
};

struct Process {
    int pid;
    string name;
    vector<Instr> instructions;
    size_t ip = 0; // instruction pointer
    map<string,uint32_t> vars;
    vector<string> logs; // print outputs
    ProcState state = P_READY;
    uint64_t wake_tick = 0; // for sleep
    mutex m;
    // for FOR loop stack: pair<repeat_count, remaining_iterations, start_ip>
    struct ForFrame { uint32_t repeats; uint32_t remaining; size_t start_ip; };
    vector<ForFrame> for_stack;
    bool attached = false; // is a user attached via screen -r
    bool finished_reported = false;
};

mutex processes_mutex;
map<int, shared_ptr<Process>> processes_by_pid;
map<string, shared_ptr<Process>> processes_by_name;
int next_pid = 1;

// ready queue contains pids
deque<int> ready_queue;
mutex ready_mutex;
condition_variable ready_cv;

// CPU simulation
uint64_t cpu_tick = 0;
mutex tick_mutex;

// scheduler threads and worker threads (simulate N CPUs)
vector<thread> cpu_workers;
thread scheduler_thread;
thread generator_thread;

// stats
mutex stats_mutex;
uint64_t total_executed_instructions = 0;
uint64_t total_processes_created = 0;

// RNG
std::mt19937 rng((uint32_t)chrono::high_resolution_clock::now().time_since_epoch().count());

// ---------- helpers ----------
string trim(const string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a==string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b-a+1);
}

vector<string> split_space(const string &s) {
    vector<string> out;
    string token;
    istringstream iss(s);
    while (iss >> token) out.push_back(token);
    return out;
}

string safe_to_name(int pid) {
    ostringstream ss;
    ss << "p" << setw(2) << setfill('0') << pid;
    return ss.str();
}

// clamp uint16
uint32_t clamp_uint16(int64_t v) {
    if (v < 0) return 0;
    if (v > 0xFFFF) return 0xFFFF;
    return (uint32_t)v;
}

// ---------- instruction generator ----------
Instr make_PRINT(const string &msg) {
    Instr i; i.opcode="PRINT"; i.args.push_back(msg); return i;
}
Instr make_DECLARE(const string &var, const string &value) {
    Instr i; i.opcode="DECLARE"; i.args.push_back(var); i.args.push_back(value); return i;
}
Instr make_ADD(const string &dst, const string &a, const string &b) {
    Instr i; i.opcode="ADD"; i.args={dst,a,b}; return i;
}
Instr make_SUB(const string &dst, const string &a, const string &b) {
    Instr i; i.opcode="SUB"; i.args={dst,a,b}; return i;
}
Instr make_SLEEP(const string &ticks) {
    Instr i; i.opcode="SLEEP"; i.args.push_back(ticks); return i;
}
Instr make_FOR_START(uint32_t repeats) {
    Instr i; i.opcode="FOR_START"; i.args.push_back(to_string(repeats)); return i;
}
Instr make_FOR_END() {
    Instr i; i.opcode="FOR_END"; return i;
}

vector<Instr> random_instructions_for(const string &pname) {
    vector<Instr> out;
    uniform_int_distribution<int> ins_count_dist((int)config.min_ins, (int)config.max_ins);
    int n = ins_count_dist(rng);
    // allow nested FOR up to 2 deep in generator
    int for_depth = 0;
    for (int i=0;i<n;i++) {
        int choice = std::uniform_int_distribution<int>(0,5)(rng);
        switch(choice) {
            case 0: {
                // PRINT (default message)
                string msg = "Hello world from " + pname + "!";
                out.push_back(make_PRINT(msg));
            } break;
            case 1: {
                // DECLARE
                string var = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                string val = to_string(std::uniform_int_distribution<int>(0,65535)(rng));
                out.push_back(make_DECLARE(var,val));
            } break;
            case 2: {
                // ADD
                string dst = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                string a = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                // second operand either var or immediate
                if (std::uniform_int_distribution<int>(0,1)(rng)==0) {
                    string b = to_string(std::uniform_int_distribution<int>(0,255)(rng));
                    out.push_back(make_ADD(dst,a,b));
                } else {
                    string b = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                    out.push_back(make_ADD(dst,a,b));
                }
            } break;
            case 3: {
                // SUB
                string dst = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                string a = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                if (std::uniform_int_distribution<int>(0,1)(rng)==0) {
                    string b = to_string(std::uniform_int_distribution<int>(0,255)(rng));
                    out.push_back(make_SUB(dst,a,b));
                } else {
                    string b = "v" + to_string(std::uniform_int_distribution<int>(1,99)(rng));
                    out.push_back(make_SUB(dst,a,b));
                }
            } break;
            case 4: {
                // SLEEP
                string ticks = to_string(std::uniform_int_distribution<int>(0,3)(rng));
                out.push_back(make_SLEEP(ticks));
            } break;
            case 5: {
                // FOR block
                if (for_depth < 2 && i+2 < n) {
                    uint32_t reps = std::uniform_int_distribution<int>(1,3)(rng);
                    out.push_back(make_FOR_START(reps));
                    for_depth++;
                    // create a small body
                    int body_len = std::uniform_int_distribution<int>(1,3)(rng);
                    for (int b=0;b<body_len;b++) {
                        out.push_back(make_PRINT("In loop " + to_string(b)));
                    }
                    out.push_back(make_FOR_END());
                    for_depth--;
                } else {
                    out.push_back(make_PRINT("Loop skipped"));
                }
            } break;
        }
    }
    return out;
}

// ---------- process creation ----------
shared_ptr<Process> create_process(const string &name) {
    auto p = make_shared<Process>();
    {
        lock_guard<mutex> lock(processes_mutex);
        p->pid = next_pid++;
        p->name = name;
    }
    p->instructions = random_instructions_for(p->name);
    p->state = P_READY;
    p->ip = 0;
    // default vars are zero (declared on use)
    {
        lock_guard<mutex> lock(ready_mutex);
        ready_queue.push_back(p->pid);
    }
    {
        lock_guard<mutex> lock(processes_mutex);
        processes_by_pid[p->pid] = p;
        processes_by_name[p->name] = p;
        total_processes_created++;
    }
    ready_cv.notify_one();
    return p;
}

// create a human readable name like p01, p02...
string create_auto_name_for_pid(int pid) {
    ostringstream ss;
    ss << "p" << setw(2) << setfill('0') << pid;
    return ss.str();
}

// ---------- CPU worker ----------
void execute_instruction_on_process(shared_ptr<Process> p) {
    lock_guard<mutex> lock(p->m);
    if (p->state == P_FINISHED) return;
    if (p->ip >= p->instructions.size()) {
        p->state = P_FINISHED;
        return;
    }

    Instr inst = p->instructions[p->ip];
    if (inst.opcode == "PRINT") {
        // append message to log
        string msg = inst.args.size()>0 ? inst.args[0] : ("Hello world from "+p->name+"!");
        p->logs.push_back(msg);
        p->ip++;
    } else if (inst.opcode == "DECLARE") {
        string var = inst.args[0];
        uint32_t val = 0;
        if (inst.args.size()>1) {
            try { val = stoi(inst.args[1]); } catch(...) { val = 0; }
        }
        p->vars[var] = clamp_uint16(val);
        p->ip++;
    } else if (inst.opcode == "ADD" || inst.opcode == "SUB") {
        string dst = inst.args[0];
        string a = inst.args[1];
        string b = inst.args[2];

        auto get_val = [&](const string &s)->uint32_t {
            if (!s.empty() && isdigit((unsigned char)s[0])) return clamp_uint16(stoi(s));
            if (p->vars.count(s)) return p->vars[s];
            p->vars[s] = 0;
            return 0;
        };

        uint32_t va = get_val(a);
        uint32_t vb = get_val(b);
        int64_t res = (inst.opcode=="ADD") ? (int64_t)va + (int64_t)vb : (int64_t)va - (int64_t)vb;
        p->vars[dst] = clamp_uint16(res);
        p->ip++;
    } else if (inst.opcode == "SLEEP") {
        uint32_t ticks = 0;
        if (inst.args.size()>0) {
            try { ticks = stoi(inst.args[0]); } catch(...) { ticks = 0; }
        }
        // set wake tick
        {
            lock_guard<mutex> tlock(tick_mutex);
            p->wake_tick = cpu_tick + ticks;
        }
        p->state = (ticks==0?P_READY:P_SLEEPING);
        p->ip++;
    } else if (inst.opcode == "FOR_START") {
        uint32_t reps = 1;
        try { reps = stoi(inst.args[0]); } catch(...) { reps=1; }
        Process::ForFrame f; f.repeats = reps; f.remaining = reps; f.start_ip = p->ip+1;
        p->for_stack.push_back(f);
        p->ip++;
    } else if (inst.opcode == "FOR_END") {
        if (!p->for_stack.empty()) {
            auto &f = p->for_stack.back();
            if (f.remaining > 1) {
                f.remaining--;
                p->ip = f.start_ip;
            } else {
                // finished loop
                p->for_stack.pop_back();
                p->ip++;
            }
        } else {
            // stray FOR_END -> skip
            p->ip++;
        }
    } else {
        // unknown/skip
        p->ip++;
    }

    // if reached end
    if (p->ip >= p->instructions.size()) p->state = P_FINISHED;
}

// Worker that simulates a CPU core: it pops from ready queue and runs one instruction (or quantum slice)
void cpu_worker_func(int cpu_id) {
    while (!stop_all) {
        int pid = -1;
        shared_ptr<Process> proc;
        {
            unique_lock<mutex> lk(ready_mutex);
            ready_cv.wait_for(lk, chrono::milliseconds(10), []{ return !ready_queue.empty() || stop_all; });
            if (stop_all) break;
            if (!ready_queue.empty()) {
                pid = ready_queue.front();
                ready_queue.pop_front();
            }
        }
        if (pid == -1) {
            // no ready process
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        {
            lock_guard<mutex> lock(processes_mutex);
            if (processes_by_pid.count(pid)) proc = processes_by_pid[pid];
            else continue;
        }
        if (!proc) continue;

        // skip finished/sleeping processes
        {
            lock_guard<mutex> lock(proc->m);
            if (proc->state == P_FINISHED) continue;
            if (proc->state == P_SLEEPING) {
                // if still sleeping push back
                lock_guard<mutex> lk(ready_mutex);
                ready_queue.push_back(pid);
                continue;
            }
            proc->state = P_RUNNING;
        }

        // Execute instructions according to scheduler
        if (config.scheduler == "rr") {
            // round robin: run up to quantum_cycles instructions (or until sleep/finish)
            uint64_t ran = 0;
            while (ran < config.quantum_cycles && !stop_all) {
                // check if sleeping due to SLEEP requested earlier
                {
                    lock_guard<mutex> tlock(tick_mutex);
                    if (proc->state == P_SLEEPING && cpu_tick < proc->wake_tick) {
                        break;
                    }
                }
                execute_instruction_on_process(proc);
                ran++;
                {
                    lock_guard<mutex> lock(stats_mutex);
                    total_executed_instructions++;
                }
                // check if finished or sleeping
                {
                    lock_guard<mutex> lock(proc->m);
                    if (proc->state == P_FINISHED || proc->state == P_SLEEPING) break;
                }
            }
        } else {
            // FCFS: run a single instruction per scheduling decision (simple)
            execute_instruction_on_process(proc);
            {
                lock_guard<mutex> lock(stats_mutex);
                total_executed_instructions++;
            }
        }

        // after executing, if still ready, requeue; if sleeping, requeue later; otherwise if finished, don't requeue
        {
            lock_guard<mutex> lock(proc->m);
            if (proc->state == P_RUNNING) proc->state = P_READY;
            if (proc->state == P_READY) {
                lock_guard<mutex> lk2(ready_mutex);
                ready_queue.push_back(proc->pid);
                ready_cv.notify_one();
            }
        }
    }
}

// ---------- scheduler thread (tick generator & sleeping management & process generator) ----------
void scheduler_loop() {
    scheduler_thread_running = true;
    while (!stop_all) {
        // increment tick
        {
            lock_guard<mutex> lk(tick_mutex);
            cpu_tick++;
        }

        // wake sleeping processes if their wake_tick passed
        {
            lock_guard<mutex> lock(processes_mutex);
            for (auto &kv : processes_by_pid) {
                auto &p = kv.second;
                lock_guard<mutex> pl(p->m);
                if (p->state == P_SLEEPING) {
                    lock_guard<mutex> tlock(tick_mutex);
                    if (cpu_tick >= p->wake_tick) {
                        p->state = P_READY;
                        lock_guard<mutex> lk2(ready_mutex);
                        ready_queue.push_back(p->pid);
                        ready_cv.notify_one();
                    }
                }
            }
        }

        // generator: when scheduler-start is running, spawn processes every batch_process_freq
        if (generator_running) {
            lock_guard<mutex> lk(tick_mutex);
            if (config.batch_process_freq > 0 && cpu_tick % config.batch_process_freq == 0) {
                // spawn new
                int pid;
                {
                    lock_guard<mutex> lock(processes_mutex);
                    pid = next_pid;
                }
                string name = create_auto_name_for_pid(pid);
                auto p = create_process(name);
                // name should be pXX; processes_by_name updated in create_process
            }
        }

        // small sleep to simulate CPU tick time
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    scheduler_thread_running = false;
}

// ---------- commands & CLI ----------
bool file_exists(const string &fn) {
    ifstream f(fn);
    return f.good();
}

bool read_config_file(const string &fn) {
    if (!file_exists(fn)) return false;
    ifstream f(fn);
    string line;
    while (getline(f, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto parts = split_space(line);
        if (parts.size() < 2) continue;
        string key = parts[0];
        string val = parts[1];
        if (key == "num-cpu") {
            try { config.num_cpu = stoi(val); if (config.num_cpu < 1) config.num_cpu = 1; if (config.num_cpu>128) config.num_cpu=128; } catch(...) {}
        } else if (key == "scheduler") {
            if (val == "fcfs" || val == "rr") config.scheduler = val;
        } else if (key == "quantum-cycles") {
            try { config.quantum_cycles = stoull(val); if (config.quantum_cycles==0) config.quantum_cycles = 1; } catch(...) {}
        } else if (key == "batch-process-freq") {
            try { config.batch_process_freq = stoull(val); if (config.batch_process_freq==0) config.batch_process_freq=1; } catch(...) {}
        } else if (key == "min-ins") {
            try { config.min_ins = stoi(val); if (config.min_ins<1) config.min_ins=1; } catch(...) {}
        } else if (key == "max-ins") {
            try { config.max_ins = stoi(val); if (config.max_ins<config.min_ins) config.max_ins = config.min_ins; } catch(...) {}
        } else if (key == "delays-per-exec") {
            try { config.delays_per_exec = stoull(val); } catch(...) {}
        }
    }
    return true;
}

string format_process_summary(shared_ptr<Process> p) {
    lock_guard<mutex> l(p->m);
    ostringstream ss;
    ss << p->name << " (pid:" << p->pid << ") ";
    if (p->state == P_FINISHED) ss << "[Finished]";
    else if (p->state == P_RUNNING) ss << "[Running]";
    else if (p->state == P_SLEEPING) ss << "[Sleeping]";
    else ss << "[Ready]";
    ss << "\n";
    return ss.str();
}

string get_screen_info(shared_ptr<Process> p) {
    lock_guard<mutex> l(p->m);
    ostringstream ss;
    ss << "Process: " << p->name << " (pid " << p->pid << ")\n";
    ss << "State: ";
    if (p->state == P_FINISHED) ss << "Finished\n"; else if (p->state==P_RUNNING) ss << "Running\n"; else if (p->state==P_SLEEPING) ss << "Sleeping\n"; else ss << "Ready\n";
    ss << "Logs:\n";
    for (auto &ln : p->logs) ss << ln << "\n";
    if (p->state == P_FINISHED) ss << "Finished!\n";
    return ss.str();
}

// Save report-util to csopesy-log.txt
void save_report_util() {
    ofstream out("csopesy-log.txt", ios::app);
    if (!out.is_open()) return;
    lock_guard<mutex> lk(stats_mutex);
    out << "=== CSOPESY UTIL REPORT at tick " << cpu_tick << " ===\n";
    out << "Num CPUs: " << config.num_cpu << " Scheduler: " << config.scheduler << " Quantum: " << config.quantum_cycles << "\n";
    out << "Total processes created: " << total_processes_created << "\n";
    out << "Total instructions executed: " << total_executed_instructions << "\n";
    // list processes
    lock_guard<mutex> lock(processes_mutex);
    out << "Processes (running/finished):\n";
    for (auto &kv : processes_by_pid) {
        auto &p = kv.second;
        lock_guard<mutex> pl(p->m);
        out << " - " << p->name << " pid:" << p->pid << " state:";
        if (p->state == P_FINISHED) out << "Finished"; else if (p->state==P_RUNNING) out << "Running"; else if (p->state==P_SLEEPING) out << "Sleeping"; else out << "Ready";
        out << " logs=" << p->logs.size() << "\n";
    }
    out << "=== END REPORT ===\n\n";
    out.close();
}

// Main command handler for MC01 commands. Returns true if this module handled the command.
bool handle_command(const string &cmd) {
    string c = trim(cmd);
    if (c.empty()) return false;
    // top-level commands: initialize, screen, scheduler-start, scheduler-stop, report-util, screen -ls, screen -s <name>, screen -r <name>
    if (c == "initialize") {
        if (initialized) {
            // already initialized
            setMessage("Already initialized\n");
            return true;
        }
        bool ok = read_config_file("config.txt");
        // if config missing, use defaults (and still mark initialized)
        initialized = true;
        // spawn scheduler and workers
        stop_all = false;
        // start scheduler thread if not started
        if (!scheduler_thread.joinable()) {
            scheduler_thread = thread(scheduler_loop);
        }
        // spawn N workers
        {
            lock_guard<mutex> lk(processes_mutex);
            // if workers exist, don't spawn duplicates
            if (cpu_workers.empty()) {
                for (uint32_t i=0;i<config.num_cpu;i++) {
                    cpu_workers.emplace_back(cpu_worker_func, (int)i);
                }
            }
        }

        setMessage("Process scheduler initialized\n");
        return true;
    }

    if (c == "scheduler-start") {
        if (!initialized) return true;
        generator_running = true;
        setMessage("Scheduler started\n");
        return true;
    }
    if (c == "scheduler-stop") {
        generator_running = false;
        setMessage("Scheduler stopped\n");
        return true;
    }
    if (c == "report-util") {
        save_report_util();
        setMessage("Report generated at csopesy-log.txt\n");
        return true;
    }
    if(c=="screen -ls"){
        stringstream ss;
        ss<<"=== PROCESS LIST ===\n";
        ss<<"Tick "<<cpu_tick<<" | CPUs "<<config.num_cpu<<" | "<<config.scheduler<<"\n\n";

        lock_guard<mutex> lk(processes_mutex);
        for(auto &kv : processes_by_pid){
            auto p = kv.second;
            lock_guard<mutex> pl(p->m);
            ss<<p->name<<" pid:"<<p->pid<<" ";
            if(p->state==P_READY) ss<<"READY\n";
            else if(p->state==P_RUNNING) ss<<"RUNNING\n";
            else if(p->state==P_SLEEPING) ss<<"SLEEPING\n";
            else ss<<"FINISHED\n";
        }

        setMessage(ss.str());
        return true;
    }
    if (c.rfind("screen -s ",0) == 0) {
        string name = trim(c.substr(10));
        if (name.empty()) {
            // invalid
            return true;
        }
        // if name already exists and running -> error
        {
            lock_guard<mutex> lock(processes_mutex);
            if (processes_by_name.count(name)) {
                // already exists
                return true;
            }
        }
        auto p = create_process(name);
        setMessage("Process "+name+" created\n");
        return true;
    }

    // screen -r <process name> -> attach to running process if exists
    if (c.rfind("screen -r ",0) == 0) {
        string name = trim(c.substr(10));
        if (name.empty()) return true;
        lock_guard<mutex> lock(processes_mutex);
        if (!processes_by_name.count(name)) {
            // print "Process <name> not found." -> The caller main should fetch output via screen_get_attached_output
            // create a ephemeral "not found" entry by adding a log?
            // we'll create a small file to indicate
            setMessage("Process "+name+" not found\n");
            ofstream out("screen-error.txt");
            out << "Process " << name << " not found.\n";
            out.close();
            return true;
        } else {
            auto p = processes_by_name[name];
            lock_guard<mutex> pl(p->m);
            if (p->state == P_FINISHED) {
                ofstream out("screen-error.txt");
                out << "Process " << name << " not found.\n";
                out.close();
                return true;
            } else {
                // mark attached; user will call screen_get_attached_output(name) to read logs
                setMessage("Attached to "+name+"\n");
                p->attached = true;
                return true;
            }
        }
    }

    // process-smi when inside a process screen: user types "process-smi" in that context.
    if (c == "process-smi") {
        // Only works if currently attached; assume user already did screen -r <name>
        // Main program calls screen_get_attached_output to fetch logs
        // but here we also print immediately
        // (Replace this placeholder file logic with console print)
        // We need to know which process is "attached"
        // We'll manage attached state inside MC01

        bool found = false;
        lock_guard<mutex> lock(processes_mutex);
        for (auto &kv : processes_by_pid) {
            auto &p = kv.second;
            lock_guard<mutex> pl(p->m);

            if (p->attached) {
                setMessage(get_screen_info(p));
                found = true;
                break;
            }
        }

        if (!found) 
           setMessage("No process is currently attached.\nUse: screen -r <name>\n");
        
        return true;
    }


    // unknown -> return false.
    return false;
}

// Return multi-line screen output for a given attached process
string screen_get_attached_output(const string &process_name) {
    lock_guard<mutex> lock(processes_mutex);
    if (process_name.empty()) {
        // return last message file if present
        ifstream in("screen-last.txt");
        if (in.good()) {
            string s((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
            return s;
        }
        return "";
    }
    if (!processes_by_name.count(process_name)) {
        return string("Process ") + process_name + " not found.\n";
    }
    auto p = processes_by_name[process_name];
    string out = get_screen_info(p);
    return out;
}

// Shutdown module
void shutdown_module() {
    // stop generator and scheduler and workers
    generator_running = false;
    stop_all = true;
    ready_cv.notify_all();
    if (scheduler_thread.joinable()) scheduler_thread.join();
    for (auto &t : cpu_workers) {
        if (t.joinable()) t.join();
    }
    cpu_workers.clear();
    // dump a final report
    save_report_util();
    initialized = false;
}

void init_module() {
    // default no config until user calls initialize command
    initialized = false;
    generator_running = false;
    scheduler_thread_running = false;
    stop_all = false;
    // nothing else now
}

// ----------------------------------------------------------------
// Expose API functions defined in header
// ----------------------------------------------------------------

} // namespace MC01

// Bridge functions (extern "C" style) - use header functions
namespace MC01 {
void init_module(); // already declared above
bool handle_command(const std::string &cmd);
std::string screen_get_attached_output(const std::string &process_name);
void shutdown_module();
} // namespace MC01
