#include <chrono>
#include <boost/asio/steady_timer.hpp>
#include "dispatcher.h"
#include "logger.h"

using namespace std;
using namespace boost;

Dispatcher::Dispatcher() {
    work_ = make_unique<boost::asio::io_service::work>(ios_);
}

Dispatcher::~Dispatcher() {

}

void Dispatcher::Run() {
    ios_.run();
}

void Dispatcher::SyncStop() {
    ios_.stop();
}

void Dispatcher::PostTask(std::function<void(void)> task) {
    ios_.post(task);
}

void Dispatcher::PostTask(std::function<void(void)> task, std::chrono::steady_clock::duration dur) {
    auto timer = make_shared<asio::steady_timer>(ios_, chrono::steady_clock::now() + dur);
    timer->async_wait([timer, task = std::move(task)](const boost::system::error_code&){
        task();
    });
}

void Dispatcher::AddSignalHandler(int sig_number, std::function<void(int)> handler) {
    auto h = make_shared<boost::asio::signal_set>(ios_, SIGINT);
    h->async_wait([h, handler](const boost::system::error_code& error , int signal_number){
        handler(signal_number);
    });
}
