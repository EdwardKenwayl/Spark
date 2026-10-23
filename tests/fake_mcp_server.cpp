#include <nlohmann/json.hpp>

#include "core/common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cctype>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc > 1 && std::string(argv[1]) == "--inspect") {
        std::vector<std::string> arguments;
        for (int index = 2; index < argc; ++index) arguments.emplace_back(argv[index]);
        std::cout << nlohmann::json{{"args", arguments}, {"cwd", spark::CurrentDirectoryUtf8()},
            {"environment", spark::EnvironmentVariable("SPARK_PROCESS_TEST_VALUE")}}.dump() << std::endl;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--pipes") {
        std::cout << std::string(100000, 'o') << std::flush;
        std::cerr << std::string(100000, 'e') << std::flush;
        return argc > 2 ? std::stoi(argv[2]) : 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--silent") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    int call_count = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            const nlohmann::json request = nlohmann::json::parse(line);
            const std::string method = request.value("method", std::string{});
            if (method == "notifications/initialized") continue;
            if (argc > 1 && std::string(argv[1]) == "--wrong-ids") {
                const nlohmann::json unrelated = {{"jsonrpc", "2.0"}, {"id", "unrelated"},
                    {"result", {{"content", nlohmann::json::array({
                        {{"type", "text"}, {"text", "wrong request"}}})}}}};
                std::cout << unrelated.dump() << std::endl;
            }
            nlohmann::json response = {{"jsonrpc", "2.0"}};
            if (request.contains("id")) response["id"] = request["id"];
            if (method == "initialize") {
                response["result"] = {
                    {"protocolVersion", "2025-06-18"},
                    {"capabilities", nlohmann::json::object()},
                    {"serverInfo", {{"name", "Spark fake MCP"}, {"version", "1.0"}}},
                };
            } else if (method == "tools/list") {
                response["result"] = {
                    {"tools", nlohmann::json::array({{
                        {"name", "echo"},
                        {"description", "Echo one string."},
                        {"inputSchema", {{"type", "object"},
                                         {"properties", {{"message", {{"type", "string"}}}}},
                                         {"required", nlohmann::json::array({"message"})}}},
                    }, {
                        {"name", "shout"},
                        {"description", "Echo one string in upper case."},
                        {"inputSchema", {{"type", "object"},
                                         {"properties", {{"message", {{"type", "string"}}}}},
                                         {"required", nlohmann::json::array({"message"})}}},
                    }})},
                };
            } else if (method == "tools/call") {
                ++call_count;
                const nlohmann::json params = request.value("params", nlohmann::json::object());
                const std::string tool_name = params.value("name", std::string{});
                std::string message = params.value("arguments", nlohmann::json::object())
                                          .value("message", std::string{});
                if (tool_name == "shout") {
                    for (char& character : message) {
                        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
                    }
                }
                response["result"] = {
                    {"content", nlohmann::json::array({{{"type", "text"},
                                                        {"text", message + "|pid=" + std::to_string(
#ifdef _WIN32
                                                            GetCurrentProcessId()
#else
                                                            getpid()
#endif
                                                        ) +
                                                                     "|count=" + std::to_string(call_count)}}})},
                    {"isError", false},
                };
            } else {
                response["error"] = {{"code", -32601}, {"message", "unknown method"}};
            }
            std::cout << response.dump() << std::endl;
        } catch (const nlohmann::json::exception& exception) {
            std::cout << nlohmann::json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", exception.what()}}}}.dump()
                      << std::endl;
        }
    }
    return 0;
}
