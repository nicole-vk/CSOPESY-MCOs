#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <sstream>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <conio.h> 

#include "final.h"

using namespace std;

bool is_running = true;
bool is_stop = false;
bool is_first_run = true;

int speed;
mutex speed_mutex;

string prompt_display_buffer = "root > ";
mutex prompt_mutex;

queue<string> command_queue;
mutex command_queue_mutex;

string marquee_display_buffer = "";
mutex marquee_display_mutex;

vector<string> ascii_text;
mutex ascii_text_mutex;

string message_text;
mutex message_mutex;

string command_line;
mutex command_line_mutex;

// ---------------- font loader ----------------
map<char, vector<string>> loadFont(const string filename) {
    map<char, vector<string>> font;
    string line;
    char currentChar = '\0';
    vector<string> buffer;

    ifstream inputFile(filename);
    if (!inputFile.is_open()) {
        cerr << "Error opening file: " << filename << endl;
        return font;
    }

    while (getline(inputFile, line)) {
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            if (currentChar != '\0') {
                font[currentChar] = buffer;
                buffer.clear();
            }
            currentChar = line[1];
        } else if (!line.empty()) {
            buffer.push_back(line);
        }
    }

    if (currentChar != '\0') {
        font[currentChar] = buffer;
    }

    inputFile.close();
    return font;
}

// ---------------- ascii maker ----------------
vector<string> makeAscii(const string text, map<char, vector<string>> font) {
    vector<string> result;
    if (text.empty() || font.empty())
        return result;

    int rows = font.begin()->second.size();
    result.assign(rows, "");

    for (int r = 0; r < rows; r++) {
        for (char c : text) {
            char up = toupper(c);
            if (font.count(up)) {
                result[r] += font[up][r] + " ";
            } else {
                result[r] += "  ";
            }
        }
    }

    return result;
}

// ---------------- keyboard handler ----------------
void keyboard_handler_thread_func() {
    while (::is_running) {
        if (_kbhit()) {
            char c = _getch();

            if (c == 0 || c == -32) {
                _getch();
                continue;
            }

            if (c == '\r') {
                string cmd;
                {
                    lock_guard<mutex> lock(command_line_mutex);
                    cmd = command_line;
                    command_line.clear();
                }
                if (!cmd.empty()) {
                    lock_guard<mutex> lock(command_queue_mutex);
                    command_queue.push(cmd);
                }
            }
            else if (c == 8) {
                lock_guard<mutex> lock(command_line_mutex);
                if (!command_line.empty()) command_line.pop_back();
            }
            else if (isprint(static_cast<unsigned char>(c))) {
                lock_guard<mutex> lock(command_line_mutex);
                command_line.push_back(c);
            }
        }
        this_thread::sleep_for(chrono::milliseconds(10));
    }
}

// ---------------- marquee logic ----------------
void marquee_logic_thread_func(int width, int height) {
    double t = 0;

    while (::is_running) {
        if (::is_stop) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        vector<string> buffer(height, string(width, ' '));

        for (int x = 0; x < width; x++) {
            float wave = 3*sin(0.12*x+t) + 2*sin(0.07*x + t*1.3);
            int waveY = int(wave + height/2);
            if (waveY >= 0 && waveY < height) buffer[waveY][x] = '~';
            if (waveY+1 >=0 && waveY+1<height) buffer[waveY+1][x] = '~';
            for (int y=waveY+2; y<waveY+7 && y<height; y++) buffer[y][x] = '.';
        }

        vector<string> local_ascii;
        {
            lock_guard<mutex> lock(ascii_text_mutex);
            local_ascii = ascii_text;
        }

        if (!local_ascii.empty()) {
            int textX = width/2;
            int textY = int(3*sin(0.12*textX+t) + 2*sin(0.07*textX+t*1.3) + height/2 - local_ascii.size());
            for (int by=0; by<(int)local_ascii.size(); by++) {
                for (int bx=0; bx<(int)local_ascii[by].size(); bx++) {
                    char ch = local_ascii[by][bx];
                    int sy = textY + by;
                    int sx = textX - (local_ascii[by].size()/2) + bx;
                    if (ch != ' ' && sy>=0 && sy<height && sx>=0 && sx<width)
                        buffer[sy][sx] = ch;
                }
            }
        }

        string combined;
        for (auto &line: buffer) combined += line + "\n";

        {
            lock_guard<mutex> lock(marquee_display_mutex);
            marquee_display_buffer = combined;
        }

        if(is_first_run)
            continue;

        t += 0.5;
        usleep(speed);
    }
}

// ---------------- display thread ----------------
void display_thread_func() {
    const int REFRESH_RATE = 50;
    const int MESSAGE_LINES = 8;

    cout << "\033[?25l";

    while (::is_running) {
        string marquee_copy, message_copy, current_line;

        {
            lock_guard<mutex> lock(marquee_display_mutex);
            marquee_copy = marquee_display_buffer;
        }

        {
            lock_guard<mutex> lock(message_mutex);
            message_copy = message_text;
        }

        {
            lock_guard<mutex> lock(command_line_mutex);
            current_line = command_line;
        }

        cout << "\033[H";
        cout << marquee_copy;

        cout << "Group developers:\n";
        cout << "Liam Michael Alain B. Ancheta\n";
        cout << "Nicole Jia Ying S. Shi\n";
        cout << "Rafael Luis L. Navarro\n";
        cout << "Reuchlin Charles S. Faustino\n\n";

        cout << "Version date: 2025-11-27\n\n";

        for (int i = 0; i < MESSAGE_LINES; ++i) {
            cout << "\033[K\n";
        }        
        cout << "\033[" << MESSAGE_LINES << "A";

        cout << "\n\033[K";
        cout << "root > " << current_line << flush;
        cout << "\n\033[K";
        cout << "\n\033[K";

        if (!message_copy.empty()) {
            cout << message_copy;
            if (message_copy.back() != '\n') cout << '\n';
        } else {
            cout << "\n";
        }

        cout << "\033[J" << flush;

        this_thread::sleep_for(chrono::milliseconds(REFRESH_RATE));
    }

    cout << "\033[?25h";
}

// ---------------- helpers ----------------
vector<string> extractCommand(const string cmd) {
    stringstream ss(cmd);
    string word;
    vector<string> words;
    while (getline(ss, word, ' ')) {
        if (!word.empty()) words.push_back(word);
    }
    return words;
}

void setMessage(const string &s) {
    lock_guard<mutex> lock(message_mutex);
    message_text = s;
}

void help_option() {
    setMessage(
        "help              - displays the commands and its description.\n"
        "initialize        - initialize the OS scheduler and memory manager.\n"
        "scheduler-start   - starts automatic process generation.\n"
        "scheduler-stop    - stops automatic process generation.\n"
        "screen -s <n> <m> - creates a process with name and memory size.\n"
        "screen -c <n> <m> \"<i>\" - creates a process with custom instructions.\n"
        "screen -r <n>     - reattaches to a process screen.\n"
        "screen -ls        - lists all processes.\n"
        "process-smi       - shows memory usage summary.\n"
        "vmstat            - shows detailed virtual memory statistics.\n"
        "report-util       - generates CPU utilization report.\n"
        "start_marquee     - starts the marquee animation.\n"
        "stop_marquee      - stops the marquee animation.\n"
        "set_text TXT      - sets marquee text.\n"
        "set_speed N       - sets marquee speed.\n"
        "show-pagetable <name> - shows the page table for a process.\n"
        "exit              - exits the console.\n"
    );
}

// ---------------- main ----------------
int main() {
    cout << "\033[2J\033[H";

    CSOPESY::init_module();

    auto font = loadFont("characters.txt");
    {
        lock_guard<mutex> lock(ascii_text_mutex);
        ascii_text = makeAscii("CSOPESY", font);
    }

    ::speed = 80000;

    const int WIDTH = 120;
    const int HEIGHT = 20;

    thread marquee_thread(marquee_logic_thread_func, WIDTH, HEIGHT);
    thread display_thread(display_thread_func);
    thread keyboard_thread(keyboard_handler_thread_func);

    while (::is_running) {
        string cmd;
        {
            lock_guard<mutex> lock(command_queue_mutex);
            if (!command_queue.empty()) {
                cmd = command_queue.front();
                command_queue.pop();
            }
        }

        if (!cmd.empty()) {
            {
                lock_guard<mutex> lock(message_mutex);
                message_text.clear();
            }

            if (cmd == "exit") {
                bool handled = CSOPESY::handle_command("exit");
                if (handled) {
                    continue;
                }

                is_running = false;
                is_stop = true;
                break;
            }
            else if (cmd == "help") help_option();
            else if (cmd == "start_marquee") {
                is_stop = false;
                is_first_run = false;
                setMessage("Marquee started\n");
            }
            else if (cmd == "stop_marquee") {
                is_stop = true;
                setMessage("Marquee stopped\n");
            }
            else if (cmd.rfind("set_text", 0) == 0) {
                string words = cmd.substr(9);
                {
                    lock_guard<mutex> lock(ascii_text_mutex);
                    ascii_text = makeAscii(words, font);
                }
                setMessage("Text set to: " + words + "\n");
            }
            else if (cmd.rfind("set_speed", 0) == 0) {
                auto parts = extractCommand(cmd);
                if (parts.size() > 1) {
                    lock_guard<mutex> speed_lock(speed_mutex);
                    ::speed = stoi(parts[1]) * 80;
                    setMessage("Speed set to: " + parts[1] + "\n");
                } else {
                    setMessage("Usage: set_speed <microseconds>\n");
                }
            }
            else {
                if (!CSOPESY::handle_command(cmd)) {
                    setMessage("Command not found. Please check the 'help' option.\n");
                }
            }
        }

        this_thread::sleep_for(chrono::milliseconds(10));
    }

    if (marquee_thread.joinable()) marquee_thread.join();
    if (display_thread.joinable()) display_thread.join();
    if (keyboard_thread.joinable()) keyboard_thread.join();

    cout << "\033[?25h";
    return 0;
}
