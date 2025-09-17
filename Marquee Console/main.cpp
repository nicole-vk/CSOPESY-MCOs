#include <iostream>
#include <vector>
#include <cmath>
#include <windows.h>
#include <thread>
#include <unistd.h> // for usleep

using namespace std;
bool isRunning = true;

bool isVirtualKeyPressed(int virKey){
    return GetAsyncKeyState(virKey) & 0x8000;       // checks if key is pressed
}

void checkKey(){
    while(isRunning){
        if(isVirtualKeyPressed(VK_DOWN)){
            cout << "shiwhdiowabdqwndqw";
            ::isRunning = false;
        }
    }
}

void animate(){
    const int width = 120;
    const int height = 20;
    double t = 0;

    // ASCII art for CSOPESY
    vector<string> csopesy = {
        R"(  ____ ____   ___  ____  _____ ______   __)",
        R"( / ___/ ___| / _ \|  _ \| ____/ ___\ \ / /)",
        R"(| |   \___ \| | | | |_) |  _| \___ \\ V / )",
        R"(| |___ ___) | |_| |  __/| |___ ___) || |  )",
        R"( \____|____/ \___/|_|   |_____|____/ |_|  )"
    };
 

    vector<string> buffer(height, string(width, ' '));

    int textX = width / 2; // center horizontally

    while (isRunning) {
        buffer.assign(height, string(width, ' '));

        // Draw waves
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

        // Clear and draw frame
        cout << "\033[H\033[J";
        for (auto &line : buffer) cout << line << endl;

        usleep(80000); // ~12 FPS
        t += 0.5;
    }


}

int main() {
    thread anim(animate);
    thread check(checkKey);

    anim.join();
    check.join();


    return 0;
}
