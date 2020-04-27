#pragma once

#include <functional>
#include <memory>
#include <chrono>
#include <boost/asio.hpp>

class Dispatcher {
public:
    Dispatcher();
    ~Dispatcher();
    void Run();
    void SyncStop();
    void PostTask(std::function<void(void)> task);
    void PostTask(std::function<void(void)>task, std::chrono::steady_clock::duration dur);
    void AddSignalHandler(int sig_number, std::function<void(int)> handler);

private:
    boost::asio::io_service ios_;
    std::unique_ptr<boost::asio::io_service::work> work_;
};