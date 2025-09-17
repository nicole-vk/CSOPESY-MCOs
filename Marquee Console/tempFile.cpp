#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
using namespace std;

map<char, vector<string>> loadFont(const string &filename) {
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


    while (getline(inputFile, line)) {
        if (line[0] == '[' && line.back() == ']') {             // check if starts with [..]
            if (currentChar != '\0') {
                font[currentChar] = buffer;
                buffer.clear();
            }
            currentChar = line[1];                              // line[1] = letter inside the bracket
        } 
        else if (!line.empty()) {
            buffer.push_back(line);
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


int main() {
    string input;

    // load characters.txt file
    auto font = loadFont("characters.txt");
    if (font.empty()) {
        cerr << "No font data loaded.\n";
        return 1;
    }

    cout << "Enter text: ";
    getline(cin, input);

    vector<string> marquee = makeAscii(input, font);makeAscii(input, font);

    for (auto &line : marquee) {
        cout << line << endl;
    }

    return 0;
}
