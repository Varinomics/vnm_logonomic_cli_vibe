// Native process fixture for version probes and the helper's vnmctl delivery.
// It never launches an interactive CLI or performs a model/network request.
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::string version = "3.1.2";
        std::ifstream selection(std::filesystem::path(argv[0]).parent_path() / "fake-version.txt");
        if (selection) std::getline(selection, version);
        std::cout << "vibe " << version << '\n';
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Run the Mistral Vibe interactive CLI\n"
                     "  --resume [SESSION_ID]\n  --continue\n  --agent AGENT\n";
        return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "provider-event") {
        const char* capture = std::getenv("VIBE_TEST_CAPTURE");
        if (!capture) return 2;
        if (std::string_view(argv[2]) == "deliver") {
            if (argc != 4 || std::string_view(argv[3]) != "--args-stdin") return 2;
            const std::string input((std::istreambuf_iterator<char>(std::cin)), {});
            std::ofstream output(capture, std::ios::binary);
            output << input;
            output.close();
            if (!output) return 3;
            if (std::getenv("VIBE_TEST_WRAPPER_FAIL")) return 1;
            if (std::getenv("VIBE_TEST_WRAPPER_STALL"))
                std::this_thread::sleep_for(std::chrono::seconds(8));
            std::cout << "{\"accepted\":true,\"epoch\":1}\n";
            return 0;
        }
        if (std::string_view(argv[2]) == "acknowledge" && argc == 4) {
            std::ofstream output(std::string(capture) + ".ack", std::ios::binary);
            output << argv[3];
            return output ? 0 : 3;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "request-title-instruction" &&
        std::string_view(argv[2]) == "--args-stdin") {
        const char* capture = std::getenv("VIBE_TEST_CAPTURE");
        if (!capture) return 2;
        const std::string input((std::istreambuf_iterator<char>(std::cin)), {});
        std::ofstream output(std::string(capture) + ".guidance", std::ios::binary | std::ios::app);
        output << input << '\n';
        output.close();
        if (!output) return 3;
        const char* selection = std::getenv("VIBE_TEST_GUIDANCE");
        const std::string_view mode = selection ? selection : "unavailable";
        if (mode == "stall") std::this_thread::sleep_for(std::chrono::seconds(8));
        if (mode == "accept")
            std::cout << "{\"accepted\":true,\"instruction_required\":true,"
                         "\"instruction\":\"Host title guidance.\\nPreserve \\\"quoted\\\" text.\"}\n";
        else if (mode == "malformed")
            std::cout << "{malformed\n";
        else
            std::cout << "{\"accepted\":false,\"reason\":\"stale_activation\"}\n";
        return 0;
    }
    // Preparation/inspection must never turn a version probe into a live run.
    std::ofstream marker(std::filesystem::path(argv[0]).parent_path() / "unexpected-launch.txt");
    marker << "unexpected invocation";
    return 93;
}
