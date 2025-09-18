#include<iostream>
#include <sstream>
#include <vector>

using namespace std;

vector<string> split_sentence(string sen) {
  
    // Create a stringstream object
    stringstream ss(sen);
    
    // Variable to hold each word
    string word;
    
    // Vector to store the words
    vector<string> words;
    
    // Extract words using getline with space as the delimiter
    while (getline(ss, word, ' ')) {
      
        // Add the word to the vector
        words.push_back(word);
    }
    
    return words;
}

int main() {
    string sen = "Geeks for Geeks";
    
    vector<string> words = split_sentence(sen);
    
    cout << words[1];
    
    return 0;
}