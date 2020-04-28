﻿#pragma once
#include <string>
#include <sstream>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/atomic.hpp>
#include <boost/thread/condition.hpp>

#include "httpclientlog.h"
#include "responseinfo.h"
#include "requestinfo.h"
#include "proxyinfo.h"
#include "httputil.h"
#include "scopedcounter.h"



//response callback: called when got all response or error
typedef boost::function<void(const ResponseInfo& r)> ResponseCallback;

//headers callback: called when got response headers
//content callback: called when got some content. non-const-reference, you can modify the content
//when return false, http will be stoppped. you can set error msg to response
//even though return false, response callback will still be called
typedef boost::function<bool(const ResponseInfo& r, std::string& error_msg)> HeadersCallback;
typedef boost::function<bool(std::string& cur_content, std::string& error_msg)> ContentCallback;



namespace
{
    inline bool default_headers_cb(const ResponseInfo& r, std::string& error_msg)
    {
        return true;
    }

    inline bool default_content_cb(std::string& cur_content, std::string& error_msg)
    {
        return true;
    }
}



//async http client class
//class is designed to make one request in one instance's lifetime
//you may get error when making request again
class CAsyncHttpClient
{
public:
    //************************************
    // brief:    constructor
    // name:     CAsyncHttpClient::CAsyncHttpClient
    // param:    boost::asio::io_service & io_service
    // param:    const unsigned short timeout           timeout setting, seconds
    // param:    const bool throw_in_cb                 if true, throw when exception in cb, otherwise, no-throw
    // return:   
    // remarks:
    //************************************
    CAsyncHttpClient(boost::asio::io_service& io_serv,
        const unsigned short timeout,
        const bool throw_in_cb = false)
        : m_io_service(io_serv)
        , m_timeout(timeout)
        , m_throw_in_cb(throw_in_cb)
        , m_sock(m_io_service)
        , m_deadline_timer(io_serv)
        , m_cb_called(false)
        , m_counter_busy(0)
        , m_method(METHOD_UNKNOWN)
    {
        m_proxy.type = PROXY_NO;
    }

    //wait for stop
    ~CAsyncHttpClient()
    {
        HTTP_CLIENT_DEBUG << "finish http";

        boost::system::error_code ec;
        m_deadline_timer.cancel(ec);
        m_sock.close(ec);

        boost::lock_guard<boost::mutex> lock_busy(m_mutex_busy);
        while (m_counter_busy)
        {
            HTTP_CLIENT_DEBUG << "wait not-busy notify";
            m_cond_busy.wait(m_mutex_busy);
            HTTP_CLIENT_DEBUG << "got not-busy notify";
        }
    }

    //set proxy stting
    void set_proxy(const ProxyInfo& proxy)
    {
        m_proxy = proxy;
    }

    //************************************
    // brief:    make request
    // name:     CAsyncHttpClient::make_request
    // param:    const RequestInfo & req
    // param:    ResponseCallback response_cb
    // param:    HeadersCallback headers_cb             optional
    // param:    ContentCallback content_cb             oprional
    // return:   void
    // remarks:
    //************************************
    void make_request(const RequestInfo& req,
        ResponseCallback response_cb,
        HeadersCallback headers_cb = default_headers_cb,
        ContentCallback content_cb = default_content_cb)
    {
        m_method = req.m_method;

        if (m_proxy.type != PROXY_NO)
        {
            m_hostname = m_proxy.server;
            m_servicename = boost::lexical_cast<std::string>(m_proxy.port);
        }
        else
        {
            m_hostname = req.gethostname();
            m_servicename = req.getservicename();
        }

        m_request_string = req.build_as_string();
        HTTP_CLIENT_DEBUG << "request_string:\r\n" << m_request_string;

        //start timeout
        m_deadline_timer.expires_from_now(boost::posix_time::seconds(m_timeout));
        m_deadline_timer.async_wait(boost::bind(&CAsyncHttpClient::timeout_cb, this,
            response_cb, boost::asio::placeholders::error));
        boost::asio::spawn(m_io_service, boost::bind(&CAsyncHttpClient::yield_func, this,
            response_cb, headers_cb, content_cb, _1));
    }


private:
    void yield_func(ResponseCallback& response_cb,
        HeadersCallback& headers_cb,
        ContentCallback& content_cb,
        boost::asio::yield_context yield)
    {
        scoped_counter_cond<int, boost::mutex, boost::condition>
            counter_cond(m_counter_busy, m_mutex_busy, m_cond_busy);

        std::string error_msg;
        do 
        {
            boost::system::error_code ec;

            boost::asio::ip::tcp::resolver rs(m_io_service);

            std::string query_host(m_hostname);
            std::string query_serivce(m_servicename);
            boost::asio::ip::tcp::resolver::query q(query_host, query_serivce);
            boost::asio::ip::tcp::resolver::iterator ep_iter = rs.async_resolve(q, yield[ec]);
            if (ec)
            {
                error_msg = "can not resolve addr which has host=";
                error_msg += query_host + " and service="
                    + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << error_msg;
                break;
            }

            boost::asio::async_connect(m_sock, ep_iter, yield[ec]);
            if (ec)
            {
                error_msg = "can not connect to addr which has host=";
                error_msg += query_host + " and service=" + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << error_msg;
                break;
            }

            boost::asio::async_write(m_sock, boost::asio::buffer(m_request_string), yield[ec]);
            if (ec)
            {
                error_msg = "can not send data to addr which has host=";
                error_msg += query_host + " and service=" + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << error_msg;
                break;
            }

            std::string content_when_header;
            {
                /*
                see http://www.boost.org/doc/libs/1_58_0/doc/html/boost_asio/reference/async_read_until/overload1.html
                After a successful async_read_until operation,
                the streambuf may contain additional data beyond the delimiter.
                An application will typically leave that data in the streambuf
                for a subsequent async_read_until operation to examine.
                */
                boost::asio::streambuf response_buf;
                boost::asio::async_read_until(m_sock, response_buf, "\r\n\r\n", yield[ec]);
                if (ec)
                {
                    error_msg = "can not recv response header, error:" + ec.message();
                    HTTP_CLIENT_ERROR << error_msg;
                    break;
                }

                std::stringstream ss;
                ss << &response_buf;
                std::string headers_contained = ss.str();
                size_t headers_pos = headers_contained.find("\r\n\r\n");
                assert(headers_pos != std::string::npos);
                std::string headers_exactly = headers_contained.substr(0, headers_pos + 4);
                content_when_header = headers_contained.substr(headers_pos + 4);

                HTTP_CLIENT_DEBUG << "response headers:\r\n" << headers_exactly;
                if (!parse_response_headers(headers_exactly, m_response))
                {
                    error_msg = "can not parse response header, invalid header:\r\n"
                        + headers_exactly;
                    HTTP_CLIENT_ERROR << error_msg;
                    break;
                }

                if (!do_headers_callback(headers_cb, error_msg))
                {
                    break;
                }
            }

            /*
            see http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
            1. check status code if 1xx, 204, and 304, check request methon if HEAD, no content
            2. check if contained Transfer-Encoding, and chunked, recv chunked content
                see https://zh.wikipedia.org/zh-cn/分块传输编码
            3. check if contained Transfer-Encoding, and no chunked, recv content until eof
            4. check if contained Content-Length, recv content with exactly length
            5. else, recv until eof
            remarks: do not consider "multipart/byteranges"
            */

            //do not use m_response.content.size() to calculate remaining data length for reading,
            //because it can be modified by user
            if ((0 == (m_response.status_code / 200) && 1 == (m_response.status_code / 100))
                || 204 == m_response.status_code
                || 304 == m_response.status_code
                || METHOD_HEAD == m_method)
            {
                HTTP_CLIENT_DEBUG <<"no content";
            }
            else if (m_response.headers.count("transfer-encoding")
                && boost::algorithm::icontains(m_response.headers["transfer-encoding"], "chunked"))
            {
                HTTP_CLIENT_DEBUG << "chunked content";

                std::string all_chunk_content;
                if (!content_when_header.empty())
                {
                    HTTP_CLIENT_DEBUG << "response chunk:\r\n" << content_when_header;
                    all_chunk_content += content_when_header;

                    std::string content;
                    if (reach_chunk_end(all_chunk_content, m_response.content))
                    {
                        do_content_callback(content_cb, error_msg);//ignore error, 'cause break to response callback
                        break;
                    }
                }

                while (true)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(1), yield[ec]);
                    if (ec)
                    {
                        if (ec != boost::asio::error::eof)
                        {
                            error_msg = ec.message();
                        }
                        break;
                    }

                    std::stringstream cur_ss;
                    cur_ss << &response_buf;
                    HTTP_CLIENT_DEBUG << "response chunk:\r\n" << cur_ss.str();
                    all_chunk_content += cur_ss.str();

                    std::string content;
                    if (reach_chunk_end(all_chunk_content, m_response.content))
                    {
                        do_content_callback(content_cb, error_msg);//ignore error, 'cause break to response callback
                        break;
                    }
                }
            }
            else if (0 == m_response.headers.count("transfer-encoding")
                && m_response.headers.count("content-length"))
            {
                HTTP_CLIENT_DEBUG << "content with content-length";

                m_response.content += content_when_header;
                if (!do_content_callback(content_cb, error_msg))
                {
                    break;
                }

                size_t content_length = boost::lexical_cast<size_t>(m_response.headers["content-length"]);
                if (content_when_header.size() < content_length)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(content_length - content_when_header.size()),
                        yield[ec]);
                    if (ec)
                    {
                        error_msg = ec.message();
                        break;
                    }

                    std::stringstream cur_ss;
                    cur_ss << &response_buf;
                    m_response.content += cur_ss.str();
                    do_content_callback(content_cb, error_msg);//ignore error, 'cause next is response callback
                }
            }
            else
            {
                HTTP_CLIENT_DEBUG << "recv content till closed";

                m_response.content += content_when_header;
                if (!do_content_callback(content_cb, error_msg))
                {
                    break;
                }

                while (true)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(1),
                        yield[ec]);
                    if (ec)
                    {
                        if (ec != boost::asio::error::eof)
                        {
                            error_msg = ec.message();
                        }
                        break;
                    }
                    std::stringstream cur_ss;
                    cur_ss << &response_buf;
                    m_response.content += cur_ss.str();
                    if (!do_content_callback(content_cb, error_msg))
                    {
                        break;//break to response callback
                    }
                }
            }

            HTTP_CLIENT_DEBUG << "response content:\r\n" << m_response.content;

        } while (false);

        do_response_callback(response_cb, error_msg);
    }


    static bool parse_response_headers(const std::string& s, ResponseInfo& r)
    {
        bool bReturn = false;
        do 
        {
            std::stringstream ss(s);
            ss >> r.http_version;
            ss >> r.status_code;
            std::getline(ss, r.status_msg);
            boost::algorithm::trim(r.status_msg);

            if (!ss)
            {
                HTTP_CLIENT_ERROR << "can not get status line";
                break;
            }

            while (ss)
            {
                std::string per_line;
                std::getline(ss, per_line);
                size_t pos = per_line.find(':');
                if (std::string::npos == pos)
                {
                    continue;
                }
                std::string key = per_line.substr(0, pos);
                boost::algorithm::trim(key);
                if (key.empty())
                {
                    HTTP_CLIENT_WARN << "encountered an empty key";
                    continue;
                }
                boost::algorithm::to_lower(key);
                std::string value = per_line.substr(pos + 1);
                boost::algorithm::trim(value);
                r.headers[key] = value;
            }

            bReturn = true;

        } while (false);

        return bReturn;
    }

    //************************************
    // brief:    check if reached ending chunk
    // name:     CAsyncHttpClient::reach_chunk_end
    // param:    std::string & all_chunk
    // param:    std::string & content          if ending, contains all content parsed from all_chunk, otherwise not-defined
    // return:   bool
    // remarks:
    //************************************
    static bool reach_chunk_end(const std::string& all_chunk, std::string& content)
    {
        content.clear();

        bool reach_end = false;
        size_t pos = 0;
        while (true)
        {
            //next \r\n
            size_t next_pos = all_chunk.find("\r\n", pos);
            if (std::string::npos == next_pos)
            {
                break;
            }
            std::string chunk_size_str = all_chunk.substr(pos, next_pos - pos);
            boost::algorithm::trim(chunk_size_str);
            if (chunk_size_str.empty())
            {
                pos = next_pos + 2;
                continue;
            }

            //chunk size
            unsigned long chunk_size = strtoul(chunk_size_str.c_str(), NULL, 16);
            if (0 == chunk_size)
            {
                reach_end = true;
                break;
            }

            content += all_chunk.substr(next_pos + 2, chunk_size);
            pos = next_pos + 2 + chunk_size + 2;//\r\n before and after
        }

        return reach_end;
    }


    void timeout_cb(ResponseCallback& response_cb, const boost::system::error_code &ec)
    {
        scoped_counter_cond<int, boost::mutex, boost::condition>
            counter_cond(m_counter_busy, m_mutex_busy, m_cond_busy);

        if (ec)
        {
            if (ec != boost::asio::error::operation_aborted)
            {
                std::string error_msg = "timeout callback encountered an error:" + ec.message();
                HTTP_CLIENT_ERROR << error_msg;
                do_response_callback(response_cb, error_msg);
            }
            else
            {
                HTTP_CLIENT_DEBUG << "timeout callback was canceled";
            }
        }
        else
        {
            HTTP_CLIENT_ERROR << "timeout";
            m_response.timeout = true;
            do_response_callback(response_cb, "");
        }
    }

    bool do_headers_callback(HeadersCallback& headers_cb, std::string& error_msg)
    {
        bool success = false;
        try
        {
            success = headers_cb(m_response, error_msg);
        }
        catch (...)
        {
            error_msg = "exception happened in headers callback function";
            HTTP_CLIENT_ERROR << error_msg;
            if (m_throw_in_cb)
            {
                HTTP_CLIENT_DEBUG << "throw";
                throw;
            }
        }

        if (!success)
        {
            HTTP_CLIENT_WARN << "headers callback execute fail, error msg: " << error_msg;
        }
        return success;
    }

    bool do_content_callback(ContentCallback& content_cb, std::string& error_msg)
    {
        bool success = false;
        try
        {
            success = content_cb(m_response.content, error_msg);
        }
        catch (...)
        {
            error_msg = "exception happened in content callback function";
            HTTP_CLIENT_ERROR << error_msg;
            if (m_throw_in_cb)
            {
                HTTP_CLIENT_DEBUG << "throw";
                throw;
            }
        }

        if (!success)
        {
            HTTP_CLIENT_WARN << "content callback execute fail, error msg: " << error_msg;
        }
        return success;
    }

    void do_response_callback(ResponseCallback& response_cb, const std::string& error_msg)
    {
        //ensure only call one time
        bool called_expected = false;
        if (m_cb_called.compare_exchange_strong(called_expected, true))
        {
            assert(!called_expected);
            boost::system::error_code ec;
            m_deadline_timer.cancel(ec);
            m_sock.close(ec);
            if (!m_response.error_msg.empty())
            {
                m_response.error_msg += "; ";
            }
            m_response.error_msg += error_msg;

            try
            {
                response_cb(m_response);
            }
            catch (...)
            {
                HTTP_CLIENT_ERROR << "exception happened in response callback function";
                if (m_throw_in_cb)
                {
                    HTTP_CLIENT_DEBUG << "throw";
                    throw;
                }
            }
        }
    }


private:
    boost::asio::io_service& m_io_service;
    const unsigned short m_timeout;
    const bool m_throw_in_cb;

    boost::asio::ip::tcp::socket m_sock;

    boost::asio::deadline_timer m_deadline_timer;
    boost::atomic_bool m_cb_called;

    boost::condition m_cond_busy;
    boost::mutex m_mutex_busy;
    int m_counter_busy;

    HTTP_METHOD m_method;
    ProxyInfo m_proxy;
    std::string m_hostname;
    std::string m_servicename;
    std::string m_request_string;

    ResponseInfo m_response;
};



