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

using namespace std;

bool is_running = true;
bool is_stop = false;

int speed;
mutex speed_mutex;

string prompt_display_buffer = "Command > ";
mutex prompt_mutex;

queue<string> command_queue;
mutex command_queue_mutex;

string marquee_display_buffer = "";
mutex marquee_display_mutex;

vector<string> ascii_text;
mutex ascii_text_mutex;


map<char, vector<string>> loadFont(const string filename) {
    map<char, vector<string>> font;
    string line;
    char currentChar = '\0';
    vector<string> buffer;

    ifstream inputFile(filename);

    // checks if file can be open
    if (!inputFile.is_open()) {
        cerr << "Error opening file: " << filename << endl;
        return font;
    }


    while (getline(inputFile, line)) {                          // read and get input from the file and load it to string line
        if (line[0] == '[' && line.back() == ']') {             // check if starts with [ and ends with ]
            if (currentChar != '\0') {
                font[currentChar] = buffer;
                buffer.clear();                                 // clear the content of previous letter
            }
            
            currentChar = line[1];                              // line[1] = letter inside the bracket
        } 
        else if (!line.empty()) {
            buffer.push_back(line);                             // append the content of the letter
        }
    }

    if (currentChar != '\0') {
        font[currentChar] = buffer;
    }

    inputFile.close();
    return font;
}


vector<string> makeAscii(const string text, map<char, vector<string>> font) {
    vector<string> result;

    if (text.empty() || font.empty()) 
        return result;

    int rows = font.begin()->second.size();                     // check the number of rows each letter has (all the same)
    result.assign(rows, "");                                    // create empty nRows

    for (int r = 0; r < rows; r++) {
        for (char c : text) {
            char up = toupper(c);                               // make it to uppercase since letter inside the bracket is capital
            if (font.count(up)) {                               // check if the letter exist
                result[r] += font[up][r] + " ";                 // append to the array
            } else {
                result[r] += "  ";
            }
        }
    }

    return result;
}


void keyboard_handler_thread_func() {
    string command_line;

    while (::is_running) {
        getline(cin, command_line);

        if (!command_line.empty()) {
            lock_guard<mutex> lock(command_queue_mutex);
            command_queue.push(command_line);
        }
    }
}


void marquee_logic_thread_func(int width, int height) {
    double t = 0;

    while (::is_running) {
        if (::is_stop) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;                                                               // not executing the code below
        }

        vector<string> buffer(height, string(width, ' '));

        // wave in animation
        for (int x = 0; x < width; x++) {
            float wave = 3*sin(0.12*x+t) + 2*sin(0.07*x + t*1.3);
            int waveY = int(wave + height/2);
            if (waveY >= 0 && waveY < height) buffer[waveY][x] = '~';
            if (waveY+1 >=0 && waveY+1<height) buffer[waveY+1][x] = '~';
            for (int y=waveY+2; y<waveY+7 && y<height; y++) buffer[y][x] = '.';
        }

        // ascii text
        int textX = width/2;
        int textY = int(3*sin(0.12*textX+t) + 2*sin(0.07*textX+t*1.3) + height/2 - ascii_text.size());
        for (int by=0; by<ascii_text.size(); by++) {
            for (int bx=0; bx<ascii_text[by].size(); bx++) {
                char ch = ascii_text[by][bx];
                int sy = textY + by;
                int sx = textX - (ascii_text[by].size()/2) + bx;

                if (ch != ' ' && sy>=0 && sy<height && sx>=0 && sx<width)
                    buffer[sy][sx] = ch;
            }
        }

        // combine into single string
        string combined = "";
        for (auto &line: buffer) combined += line + "\n";

        {
            lock_guard<mutex> lock(marquee_display_mutex);
            marquee_display_buffer = combined;
        }

        t += 0.5;
        usleep(speed);
    }
}



void display_thread_func() {
    const int REFRESH_RATE = 50;
    while (::is_running) {
        string marquee_copy;
        string prompt_copy;

        {
            lock_guard<mutex> lock1(marquee_display_mutex);
            marquee_copy = marquee_display_buffer;
        }

        {
            lock_guard<mutex> lock2(prompt_mutex);
            prompt_copy = prompt_display_buffer;
        }




        /*
        
        BELOW IS THE PROBLEM....
        
        
        */


        cout << "\033[0;0H";
        cout << marquee_copy;
        cout << "\033[25;0H" << prompt_copy << flush;

        this_thread::sleep_for(chrono::milliseconds(REFRESH_RATE));
    }
}

vector<string> extractCommand(string cmd){
    stringstream ss(cmd);
    string word;
    vector<string> words;
    
    while (getline(ss, word, ' ')) {        // space as the delimiter
        words.push_back(word);
    }
    
    return words;
}

void help_option(){
    cout << "help          - displays the commands and its description." << endl;
    cout << "start_marquee - starts the marquee animation." << endl;
    cout << "stop_marquee  - stops the marquee animation." << endl ;
    cout << "set_text      - accepts a text input and displays it as a marquee." <<  endl;
    cout << "set_speed     - sets the marquee animation refresh in milliseconds." <<  endl;
    cout << "exit          - terminates the console." <<  endl;
}

int main() {
    cout << "\033[2J\033[H";                                            // clear screen

    auto font = loadFont("characters.txt");
    ::ascii_text = makeAscii("CSOPESY", font);                          // default ascii
    ::speed = 80000;                                                    // default speed


    // width and height of the animation thread
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
            lock_guard<mutex> lock(prompt_mutex);
            prompt_display_buffer = "Command > " + cmd;
            
            if (cmd == "exit"){
                ::is_running = false;
                exit(0);
            }
            else if(cmd == "help")
                help_option();
            else if(cmd == "start_marquee")
                ::is_stop = false;
            else if(cmd == "stop_marquee")
                ::is_stop = true;
            else if(cmd.rfind("set_text", 0) == 0){
                vector<string> words = extractCommand(cmd);
                
                lock_guard<mutex> lock(ascii_text_mutex);
                ::ascii_text = makeAscii(words[1], font);
            }
            else if(cmd.rfind("set_speed", 0) == 0){
                vector<string> words = extractCommand(cmd);
                
                lock_guard<mutex> speed_lock(speed_mutex);
                ::speed = stoi(words[1]);                           // convert string to int
            }
            else
                cout << "Command not found. Please check the 'help' option.";
        }

        this_thread::sleep_for(chrono::milliseconds(10));
    }

    // join threads
    if (marquee_thread.joinable()) marquee_thread.join();
    if (display_thread.joinable()) display_thread.join();
    if (keyboard_thread.joinable()) keyboard_thread.join();

    return 0;
}