﻿#define HAS_HTTP_CLIENT_LOG
#include <boost/thread.hpp>
#include <boost/locale.hpp>
#include "asynchttpclient.h"
#include "synchttpclient.h"
#include "asyncdownload.h"
#include "syncdownload.h"


boost::asio::io_service g_io_service;
ProxyInfo g_proxy;


void handle_response(const ResponseInfo& r)
{
    if (r.timeout)
    {
        std::cout << "timeout" << std::endl;
    }
    else if (!r.error_msg.empty())
    {
        std::cout << r.error_msg.c_str() << std::endl;
    }
    else if (-1 == r.status_code)
    {
        std::cout << "unknown error" << std::endl;
    }
    else if (200 != r.status_code)
    {
        std::cout << "http error: " << r.status_code << ", " << r.status_msg << std::endl;
    }
    else
    {
        std::cout << "response size=" << r.content.size()
            << ", content:\r\n" << r.content.c_str() << std::endl;
    }
}


void cb_async_http(boost::shared_ptr<CAsyncHttpClient>& pClient,
                   const ResponseInfo& r)
{
    handle_response(r);
}


void thread_async()
{
    //while (true)
    {
        RequestInfo req;
        req.set_url(boost::locale::conv::from_utf<wchar_t>(L"http://www.baidu.com/我在这里", "utf8"));
        req.set_method(METHOD_GET);
        boost::shared_ptr<CAsyncHttpClient> pClient
            = boost::make_shared<CAsyncHttpClient>(boost::ref(g_io_service), 5);
        //pClient->set_proxy(g_proxy);
        pClient->make_request(req, boost::bind(cb_async_http, pClient, _1));
        boost::this_thread::sleep(boost::posix_time::seconds(1));
    }
}


void test_sync()
{
    //while (true)
    {
        RequestInfo req;
        req.set_url("http://www.baidu.com/123");
        req.set_method(METHOD_GET);
        CSyncHttpClient client(5);
        //client.set_proxy(g_proxy);
        handle_response(client.make_request(req));
        boost::this_thread::sleep(boost::posix_time::seconds(1));
    }
}


void test_async()
{
    boost::thread t(thread_async);
    {
        boost::asio::io_service::work work(g_io_service);
        g_io_service.run();
    }
}


void cb_async_download(boost::shared_ptr<CAsyncHttpDownload>& pClient,
                       const ResponseInfo& r)
{
    handle_response(r);
}


void download_async()
{
    //while (true)
    {
        RequestInfo req;
        req.set_url("http://www.baidu.com/123");
        req.set_method(METHOD_GET);
        boost::shared_ptr<CAsyncHttpDownload> pClient
            = boost::make_shared<CAsyncHttpDownload>(boost::ref(g_io_service), 5);
        //pClient->set_proxy(g_proxy);
        pClient->download(req, "D:\\test_download.txt",
            boost::bind(cb_async_download, pClient, _1));
        boost::this_thread::sleep(boost::posix_time::seconds(1));
    }
}


void test_async_download()
{
    boost::thread t(download_async);
    {
        boost::asio::io_service::work work(g_io_service);
        g_io_service.run();
    }
}


void test_sync_download()
{
    //while (true)
    {
        RequestInfo req;
        req.set_url("http://www.baidu.com/123");
        req.set_method(METHOD_GET);
        CSyncHttpDownload client(5);
        //client.set_proxy(g_proxy);
        handle_response(client.download(req, "D:\\test_download.txt"));
        boost::this_thread::sleep(boost::posix_time::seconds(1));
    }
}


int main()
{
    g_proxy.type = PROXY_HTTP;
    g_proxy.server = "127.0.0.1";
    g_proxy.port = 8888;

    test_async();
    return 0;
}



