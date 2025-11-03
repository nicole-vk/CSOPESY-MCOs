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

extern void setMessage(const std::string &s);
using namespace std;

namespace MC01 {

struct Config {
    uint32_t num_cpu = 1;
    string scheduler = "fcfs";
    uint64_t quantum_cycles = 1;
    uint64_t batch_process_freq = 1;
    uint32_t min_ins = 1;
    uint32_t max_ins = 8;
    uint64_t delays_per_exec = 0;
} config;

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

std::mt19937 rng((uint32_t)chrono::high_resolution_clock::now().time_since_epoch().count());

// atomic busy-state per core: MUST NOT be std::vector<atomic<bool>> (copy/move issue)
vector<bool> core_busy;
atomic<uint64_t> busy_core_count(0);

// ---------------- Helpers ----------------

string trim(const string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"); 
    if (a==string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b-a+1);
}

vector<string> split_space(const string &s) {
    vector<string> o; string t; istringstream is(s);
    while (is >> t) o.push_back(t);
    return o;
}

uint32_t clamp_uint16(int64_t v) {
    if (v < 0) return 0;
    if (v > 0xFFFF) return 0xFFFF;
    return (uint32_t)v;
}

// ---------------- Instruction builders ----------------
Instr make_PRINT(const string &msg){Instr i;i.opcode="PRINT";i.args={msg};return i;}
Instr make_ADD(const string &d,const string &a,const string &b){Instr i;i.opcode="ADD";i.args={d,a,b};return i;}

vector<Instr> random_instructions_for(const string &pname) {
    uniform_int_distribution<int> d(config.min_ins, config.max_ins);
    int n = d(rng);
    if (n < 2) n = 2;
    vector<Instr> out;
    for (int i=0;i<n;i++) {
        if (i % 2 == 0)
            out.push_back(make_PRINT("Hello world from " + pname + "!"));
        else
            out.push_back(make_ADD("x","x",to_string(uniform_int_distribution<int>(1,10)(rng))));
    }
    return out;
}

// ---------------- Process creation ----------------

shared_ptr<Process> create_process(const string &name) {
    auto p = make_shared<Process>();

    {
        lock_guard<mutex> lk(processes_mutex);
        p->pid = next_pid++;
        p->name = name;
    }

    p->vars["x"]=0;
    p->instructions = random_instructions_for(p->name);

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

string auto_name(int pid) {
    ostringstream ss;
    ss<<"p"<<setw(2)<<setfill('0')<<pid;
    return ss.str();
}

string join_last(const vector<string>& v,int last=10){
    stringstream ss;
    int s=max(0,(int)v.size()-last);
    for(int i=s;i<(int)v.size();i++) ss<<v[i]<<"\n";
    return ss.str();
}

// ---------------- Execution ----------------

void exec_inst(shared_ptr<Process> p,int cid){
    lock_guard<mutex> lk(p->m);
    if(p->state==P_FINISHED||p->ip>=p->instructions.size()){p->state=P_FINISHED;return;}

    auto &in=p->instructions[p->ip];

    if(in.opcode=="PRINT"){
        auto now = chrono::system_clock::now();
        time_t t= chrono::system_clock::to_time_t(now);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm,&t);
#else
        localtime_r(&t,&tm);
#endif
        char b[64];
        strftime(b,64,"(%m/%d/%Y %I:%M:%S%p)",&tm);
        ostringstream m;
        m<<b<<" Core:"<<cid<<" \""<<in.args[0]<<"\"";
        p->logs.push_back(m.str());
        p->ip++;
    }
    else if(in.opcode=="ADD"){
        auto get=[&](string s){
            if(isdigit(s[0]))return(uint32_t)stoi(s);
            if(!p->vars.count(s))p->vars[s]=0;
            return p->vars[s];
        };
        int64_t r=(int64_t)get(in.args[1])+(int64_t)get(in.args[2]);
        p->vars[in.args[0]]=clamp_uint16(r);
        p->ip++;
    }

    if(p->ip>=p->instructions.size())p->state=P_FINISHED;
}

// ---------------- Worker Thread ----------------

void cpu_worker(int cid){
    while(!stop_all){
        int pid=-1;
        {
            unique_lock<mutex> lk(ready_mutex);
            if(ready_queue.empty()){
                lk.unlock();
                this_thread::sleep_for(chrono::milliseconds(1));
                continue;
            }
            pid=ready_queue.front(); ready_queue.pop_front();
        }

        shared_ptr<Process> p;
        {
            lock_guard<mutex> lk(processes_mutex);
            if(!processes_by_pid.count(pid)) continue;
            p=processes_by_pid[pid];
        }

        {
            lock_guard<mutex> lk(p->m);
            if(p->state==P_FINISHED) continue;
            p->state = P_RUNNING;
        }

        if(!core_busy[cid]){core_busy[cid]=true;busy_core_count++;}

        if(config.scheduler=="rr"){
            for(uint64_t i=0;i<config.quantum_cycles;i++){
                exec_inst(p,cid);
                total_executed_instructions++;
                if(p->state!=P_RUNNING) break;
            }
        }else{
            exec_inst(p,cid);
            total_executed_instructions++;
        }

        {
            lock_guard<mutex> lk(p->m);
            if(p->state==P_RUNNING) p->state=P_READY;
        }

        {
            lock_guard<mutex> lk(ready_mutex);
            if(p->state==P_READY) ready_queue.push_back(pid);
        }

        if(core_busy[cid]){core_busy[cid]=false;busy_core_count--;}
    }
}

// ---------------- Scheduler ----------------

void scheduler_loop(){
    while(!stop_all){
        {
            lock_guard<mutex> lk(tick_mutex);
            cpu_tick++;
        }
        if(generator_running && config.batch_process_freq>0 && cpu_tick % config.batch_process_freq == 0){
            int pid;
            {
                lock_guard<mutex> lk(processes_mutex);
                pid=next_pid;
            }
            create_process(auto_name(pid));
        }
        this_thread::sleep_for(10ms);
    }
}

// ---------------- Config ----------------

bool read_config_file(const string& fn){
    ifstream f(fn); if(!f.good()) return false;
    string l;
    while(getline(f,l)){
        l=trim(l); if(l.empty()) continue;
        auto p=split_space(l); if(p.size()<2) continue;
        string k=p[0],v=p[1];
        try{
            if(k=="num-cpu")config.num_cpu=max(1,stoi(v));
            else if(k=="scheduler")config.scheduler=v;
            else if(k=="quantum-cycles")config.quantum_cycles=max(1ULL,stoull(v));
            else if(k=="batch-process-freq")config.batch_process_freq=max(1ULL,stoull(v));
            else if(k=="min-ins")config.min_ins=stoi(v);
            else if(k=="max-ins")config.max_ins=stoi(v);
            else if(k=="delays-per-exec")config.delays_per_exec=stoull(v);
        }catch(...){}
    }
    return true;
}

// ---------------- Commands ----------------

bool handle_command(const string &cmd){
    string c=trim(cmd);
    if(c=="initialize"){
        if(initialized){setMessage("Already initialized\n");return true;}
        read_config_file("config.txt");
        initialized=true; stop_all=false;
        core_busy.assign(config.num_cpu,false);
        if(!scheduler_thread.joinable()) scheduler_thread=thread(scheduler_loop);
        for(uint32_t i=0;i<config.num_cpu;i++)
            cpu_workers.emplace_back(cpu_worker,i);
        setMessage("Process scheduler initialized\n");
        return true;
    }

    if(c=="scheduler-start"){ generator_running=true; setMessage("Scheduler started\n");return true; }
    if(c=="scheduler-stop"){ generator_running=false; setMessage("Scheduler stopped\n");return true; }

    if(c=="screen -ls"){
        stringstream ss;
        uint64_t busy = busy_core_count.load();
        double util = (config.num_cpu>0)?(100.0*busy/config.num_cpu):0;

        ss<<"=== SCREEN - LIST ===\n";
        ss<<"CPU utilization: "<<(int)util<<"%\n";
        ss<<"Cores used: "<<busy<<"\n";
        ss<<"Cores available: "<<(config.num_cpu-(int)busy)<<"\n\n";

        ss<<"Running processes:\n";

        vector<shared_ptr<Process>> finished;

        {
            lock_guard<mutex> lock(processes_mutex);
            for(auto &kv:processes_by_pid){
                auto p = kv.second;
                lock_guard<mutex> pl(p->m);
                if(p->state==P_RUNNING||p->state==P_READY||p->state==P_SLEEPING){
                    uint32_t core = p->vars.count("_core")?p->vars["_core"]:0xFFFFFFFF;
                    ss<<p->name<<"  Core:"<<(core==0xFFFFFFFF?string("-"):to_string(core))
                      <<"  "<<p->ip<<" / "<<p->instructions.size()<<"\n";
                }
                else if(p->state==P_FINISHED) finished.push_back(p);
            }
        }

        sort(finished.begin(),finished.end(),[](auto&a,auto&b){return a->pid<b->pid;});
        if(finished.size()>20) finished.erase(finished.begin(), finished.end()-20);

        ss<<"\nFinished processes (last 20):\n";
        for(auto&p:finished)
            ss<<p->name<<"  Finished "<<p->instructions.size()<<" / "<<p->instructions.size()<<"\n";

        setMessage(ss.str());
        return true;
    }

    if(c.rfind("screen -s ",0)==0){
        string name=trim(c.substr(10));
        if(name.empty()){setMessage("Invalid name\n");return true;}
        if(processes_by_name.count(name)){setMessage("Process already exists\n");return true;}
        create_process(name);
        setMessage("Process "+name+" created\n");
        return true;
    }

    return false;
}

void shutdown_module(){
    stop_all=true;
    ready_cv.notify_all();
    if(scheduler_thread.joinable()) scheduler_thread.join();
    for(auto&t:cpu_workers) if(t.joinable()) t.join();
    cpu_workers.clear();
    initialized=false;
}

void init_module(){
    initialized=false;
    generator_running=false;
    stop_all=false;
    core_busy.clear();
    busy_core_count.store(0);
}

}
