#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "glance/contracts/cli_protocol.h"

namespace glance::app
{
    class CommandServer
    {
    public:
        using Handler = std::function<std::string(std::string, HANDLE, HANDLE)>;
        using Replied = std::function<void(std::string_view)>;
        CommandServer(Handler handler, Replied replied);
        ~CommandServer();
        void stop() noexcept;
    private:
        void run() noexcept;
        Handler handler_;
        Replied replied_;
        glance::cli::Handle stop_;
        std::vector<std::thread> workers_;
    };
}
