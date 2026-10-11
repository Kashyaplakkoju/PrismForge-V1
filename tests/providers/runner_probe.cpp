#include <chrono>
#include <string>
#include <thread>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::string(argv[1]) == "exit19") return 19;
    if (std::string(argv[1]) == "timeout") { std::this_thread::sleep_for(std::chrono::seconds(10)); return 0; }
    return 2;
}
