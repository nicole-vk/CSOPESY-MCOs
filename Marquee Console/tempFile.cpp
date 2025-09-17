#include <iostream>
#include <fstream>
#include <vector>

using namespace std;
int main(){
    string key = "[";
    vector<string> asciiArr;
    string line;
    int times = 1;

    ifstream inputFile("characters.txt");

    if(!inputFile.is_open()){
        cerr << "Error opening the file.\n";
        return 1;
    }

    while(getline(inputFile, line) && line != "\0"){
        size_t found = line.rfind(key);

        if(found == std::string::npos){
            if (line != "\0"){
                asciiArr.push_back(line);
        }
        }
    }

    for(string ascii : asciiArr){
        if(times < 7){
            cout << ascii << endl;
            times++;
        }
        else
            times = 0;
    }

    inputFile.close();
    return 0;
}