#include <endian.h>
#include <iostream>
#include <fstream>

#include <thread>
#include <chrono>


int read() {
    std::ifstream hc_sr04_in("/dev/hc-sr04");
    if (!hc_sr04_in.is_open()) {
        std::cerr << "Could not open /dev/hc-sr04" << std::endl;
        return -1;
    }

    uint32_t data;
    if (hc_sr04_in.read((char*) &data, sizeof(data))) {
        return data;
    }
    else {
        std::cerr << "Read failed" << std::endl;
    }

    return -1;
}

int main() {

    while (true) {
        auto range_mm = read();
        if (range_mm != -1) {
            std::cout << "Measured range in mm: " << range_mm << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}