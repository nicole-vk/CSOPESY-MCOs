#include <iostream>
#include <vector>
#include <cmath>
#include <windows.h>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <map>
#include <string>

using namespace std;
bool isRunning = true;
const string fileName = "characters.txt";

bool isVirtualKeyPressed(int virKey){
    return GetAsyncKeyState(virKey) & 0x8000;       // checks if key is pressed
}

void checkKey(){
    while(::isRunning){
        if(isVirtualKeyPressed(VK_DOWN)){
            cout << "shiwhdiowabdqwndqw";
            ::isRunning = false;
        }
    }
}

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


vector<string> makeAscii(const string &text, map<char, vector<string>> &font) {
    vector<string> result;

    if (text.empty() || font.empty()) return result;

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

void animate(map<char, vector<string>> font){
    const int width = 120;
    const int height = 20;
    double t = 0;

    // ASCII art for CSOPESY
    vector<string> csopesy = makeAscii("CSOPESY", font);
    
    vector<string> buffer(height, string(width, ' '));

    int textX = width / 2; // center horizontally

    while (::isRunning) {
        buffer.assign(height, string(width, ' '));

        // draw waves
        for (int x = 0; x < width; x++) {
            float wave = 3 * sin(0.12 * x + t) + 2 * sin(0.07 * x + t * 1.3);
            int waveY = (int)(wave + height / 2);

            if (waveY >= 0 && waveY < height) buffer[waveY][x] = '~';
            if (waveY + 1 >= 0 && waveY + 1 < height) buffer[waveY + 1][x] = '~';

            for (int y = waveY + 2; y < height; y++) buffer[y][x] = '.';
        }

        // CSOPESY text movement (riding the wave like the ball)
        float waveText = 3 * sin(0.12 * textX + t) + 2 * sin(0.07 * textX + t * 1.3);
        int textY = (int)(waveText + height / 2 - csopesy.size());

        for (int by = 0; by < csopesy.size(); by++) {
            for (int bx = 0; bx < csopesy[by].size(); bx++) {
                char ch = csopesy[by][bx];
                int sy = textY + by;
                int sx = textX - (csopesy[by].size() / 2) + bx;

                if (ch != ' ' && sy >= 0 && sy < height && sx >= 0 && sx < width) {
                    buffer[sy][sx] = ch;
                }
            }
        }

        // clear and draw frame
        cout << "\033[H\033[J";
        for (auto &line : buffer) cout << line << endl;

        usleep(80000); // ~12 FPS
        t += 0.5;
    }
}

void commands(){
    string command;

    while(::isRunning){
        cout << "command > ";
        getline(cin, command);

        cout << command << endl;
    }
}


int main() {
    auto font = loadFont(::fileName);

    thread anim(animate, font);
    thread command(commands);
    thread check(checkKey);

    anim.join();
    command.join();
    check.join();


    return 0;
}
