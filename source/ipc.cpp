/**
 * Lightweight inter process communication library
 * Copyright (C) 2020 Pavel Kovalenko 
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License, v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <filesystem>
#include <string.h>
#include <thread>

#include "../include/ipc.hpp"

namespace ipc
{
    static bool init_socket_api() noexcept
    {
#ifdef _WIN32
        struct WSAInit_t
        {
            bool m_ok;
            WSAInit_t()
            {
                WSADATA wsaData;
                int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
                m_ok = (iResult == NO_ERROR);
            }
        };

        static WSAInit_t init;
        return init.m_ok;
#else
        return true;
#endif
    }

    socket::socket(socket_t socket) : m_ok(init_socket_api()), m_socket(socket)
    {
        if (!m_ok)
            throw socket_api_failed_exception(get_socket_error(), __FUNCTION_NAME__);
    }

    void socket::close() noexcept
    {
        if (m_socket != INVALID_SOCKET)
        {
#ifdef _WIN32
            closesocket(m_socket);
#else
            ::close(m_socket);
#endif
            m_socket = INVALID_SOCKET;
            m_ok = false;
        }
    }

    static bool set_non_blocking_mode(socket_t s) noexcept
    {
#ifdef _WIN32
        u_long iMode = 1;
        return (ioctlsocket(s, FIONBIO, &iMode) == NO_ERROR);
#else
        int flags = fcntl(s, F_GETFL);
        return (fcntl(s, F_SETFL, flags | O_NONBLOCK) >= 0);
#endif
    }

#ifdef __AFUNIX_H__
    template <typename T, typename>
    unix_server_socket::unix_server_socket(T&& socket_link) : m_link(std::forward<T>(socket_link))
    {
        m_socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (INVALID_SOCKET == m_socket)
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to allocate socket");

        sockaddr_un serv_addr;
        serv_addr.sun_family = AF_UNIX;
        strcpy(serv_addr.sun_path, m_link.c_str());

        if (!set_non_blocking_mode(m_socket))
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to enable non blocking mode");

        if (bind(m_socket, (struct sockaddr*)&serv_addr, offsetof(sockaddr_un, sun_path) + strlen(serv_addr.sun_path)) != 0)
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to bind socket");

        if (listen(m_socket, 100) != 0)
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to listen socket");
    }

    template unix_server_socket::unix_server_socket(const std::string&);
    template unix_server_socket::unix_server_socket(std::string&&);

    void unix_server_socket::close() noexcept
    {
        super::close();
        if (!m_link.empty())
            unlink(m_link.c_str());
    }

#endif //__AFUNIX_H__

#ifdef _WIN32
    static inline bool is_socket_exists(const char* s) noexcept
    {
        return (GetFileAttributesA(s) != INVALID_FILE_ATTRIBUTES);
    }
#else
    static inline bool is_socket_exists(const char* s) noexcept
    {
        std::error_code ec;
        return std::filesystem::exists(s, ec);
    }
#endif

#ifdef __AFUNIX_H__
    unix_client_socket::unix_client_socket(const char* path) : point_to_point_socket(INVALID_SOCKET)
    {
        m_socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (INVALID_SOCKET == m_socket)
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to allocate socket");

        sockaddr_un serv_addr;
        serv_addr.sun_family = AF_UNIX;

        if (!is_socket_exists(path))
        {
#ifdef _WIN32
            int ecode = ERROR_FILE_NOT_FOUND;
#else
            int ecode = ENOENT;
#endif
            fail_status<socket_prepare_exception>(m_ok, ecode, std::string(__FUNCTION_NAME__) + ": target does not exist");
        }

        strncpy(serv_addr.sun_path, path, sizeof(serv_addr.sun_path));

        const int max_attempts_count = 10;
        int attempt = 0;
        for (; attempt < max_attempts_count && connect(m_socket, (struct sockaddr*)&serv_addr, offsetof(sockaddr_un, sun_path) + strlen(serv_addr.sun_path)) < 0; ++attempt)
        {
            int err_code = get_socket_error();
#ifdef _WIN32
            if (err_code == WSAECONNREFUSED)
#else
            if (err_code == EAGAIN || err_code == ECONNREFUSED || err_code == EINPROGRESS)
#endif
            {
                std::this_thread::sleep_for(std::chrono::seconds(1)); // TODO: fix me
            }
            else
                fail_status<socket_prepare_exception>(m_ok, err_code, std::string(__FUNCTION_NAME__) + ": unable to connect");
        }

        if (attempt == max_attempts_count)
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to connect");

        if (!set_non_blocking_mode(m_socket))
            fail_status<socket_prepare_exception>(m_ok, get_socket_error(), std::string(__FUNCTION_NAME__) + ": unable to enable non blocking mode");
    }

#endif //__AFUNIX_H__

    [[noreturn]] void ipc::throw_message_overflow_exception(const char* func_name, size_t req_size, size_t total_size)
    {
        std::string msg(func_name);
        msg.append(": required space ").append(std::to_string(req_size));
        msg.append("exceeds limit of ").append(std::to_string(total_size)).append(" bytes");
        throw message_overflow_exception(std::move(msg));
    }

    [[noreturn]] void ipc::throw_type_mismatch_exception(const char* func_name, const char* tag, const char* expected)
    {
        std::string msg(func_name);
        msg.append(": data type mismatch (got ").append(tag).append(", expect ").append(expected).append(")");
        throw type_mismach_exception(std::move(msg));
    }

    [[noreturn]] void throw_message_too_short_exception(const char* func_name, size_t req_size, size_t total_size)
    {
        std::string msg(func_name);
        msg.append(": required space ").append(std::to_string(req_size));
        msg.append("exceeds message length of ").append(std::to_string(total_size)).append(" bytes");
        throw message_too_short_exception(std::move(msg));
    }

    [[noreturn]] void ipc::throw_container_overflow_exception(const char* func_name, size_t req_size, size_t total_size)
    {
        std::string msg(func_name);
        msg.append(": required space ").append(std::to_string(req_size));
        msg.append("exceeds container limit of ").append(std::to_string(total_size)).append(" bytes");
        throw container_overflow_exception(std::move(msg));
    }

#ifdef __AFUNIX_H__
    constexpr const char* ipc::message::to_string(type_tag t) noexcept
    {
        switch (t)
        {
        case ipc::message::type_tag::u32:
            return "u32";
        case ipc::message::type_tag::i32:
            return "i32";
        case ipc::message::type_tag::u64:
            return "u64";
        case ipc::message::type_tag::i64:
            return "i64";
        case ipc::message::type_tag::fp64:
            return "fp64";
        case ipc::message::type_tag::str:
            return "str";
        case ipc::message::type_tag::chr:
            return "chr";
        case ipc::message::type_tag::remote_ptr:
            return "remote_ptr";
        case ipc::message::type_tag::blob:
            return "blob";
        default:
            return "unknown";
        }
    }
#endif // __AFUNIX_H__

    out_message& out_message::operator << (const std::string_view& s)
    {
        check_status<bad_message_exception>(m_ok, std::string(__FUNCTION_NAME__) + ": fail flag is set");

#if __MSG_USE_TAGS__
        const size_t delta = 2; // terminating '\0' and tag
#else
        const size_t delta = 1; // terminating '\0' only
#endif // __MSG_USE_TAGS__
        const char* arg = s.data();
        const size_t len = s.length();
        const size_t used = *(__MSG_LENGTH_TYPE__*)m_buffer.data();
        const size_t new_used = used + len + delta;
        if (new_used > get_max_size())
            fail_status(throw_message_overflow_exception, m_ok, __FUNCTION_NAME__, new_used, get_max_size());
        else
        {
#if __MSG_USE_TAGS__
            m_buffer.push_back((char)type_tag::str);
#endif // __MSG_USE_TAGS__
            m_buffer.insert(m_buffer.end(), arg, arg + len);
            m_buffer.push_back('\0'); // string_view is not necessarily null terminated, so we set it explicitly
            *(__MSG_LENGTH_TYPE__*)m_buffer.data() += (__MSG_LENGTH_TYPE__)new_used;
        }
        
        return *this;
    }

    out_message& out_message::operator << (const std::pair<const uint8_t*, size_t>& blob)
    {
        check_status<bad_message_exception>(m_ok, std::string(__FUNCTION_NAME__) + ": fail flag is set");

#if __MSG_USE_TAGS__
        const size_t delta = 1;
#else
        const size_t delta = 0;
#endif // __MSG_USE_TAGS__
        const uint8_t* arg = blob.first;
        const size_t len = blob.second;
        const size_t used = *(__MSG_LENGTH_TYPE__*)m_buffer.data();
        const size_t new_used = used + len + delta;
        if (new_used > get_max_size())
            fail_status(throw_message_overflow_exception, m_ok, __FUNCTION_NAME__, new_used, get_max_size());
        else
        {
#if __MSG_USE_TAGS__
            m_buffer.push_back((char)type_tag::blob);
#endif // __MSG_USE_TAGS__
            const __MSG_LENGTH_TYPE__ blob_len = (__MSG_LENGTH_TYPE__)len;
            m_buffer.insert(m_buffer.end(), (const char*)&blob_len, (const char*)(&blob_len + 1));
            m_buffer.insert(m_buffer.end(), arg, arg + len);
            *(__MSG_LENGTH_TYPE__*)m_buffer.data() += (__MSG_LENGTH_TYPE__)new_used;
        }

        return *this;
    }

    in_message& in_message::operator >> (std::string& arg)
    {
        check_status<bad_message_exception>(m_ok, std::string(__FUNCTION_NAME__) + ": fail flag is set");

        arg.clear();
        __MSG_LENGTH_TYPE__ size = *(__MSG_LENGTH_TYPE__*)m_buffer.data();
#if __MSG_USE_TAGS__
        const size_t delta = 2; /*termination '\0' and type tag*/
#else
        const size_t delta = 1; /*termination '\0' only*/
#endif // __MSG_USE_TAGS__
        if (size < m_offset + delta)
            fail_status(throw_message_too_short_exception, m_ok, __FUNCTION_NAME__, m_offset + delta, size);

#if __MSG_USE_TAGS__
        type_tag tag = (type_tag)m_buffer[m_offset];
        if (tag != type_tag::str)
            fail_status(throw_type_mismatch_exception, m_ok, __FUNCTION_NAME__, to_string(tag), to_string(type_tag::str));

        ++m_offset;
#endif // __MSG_USE_TAGS__

        const char* begin = &m_buffer[m_offset];
        const char* end = (const char*)memchr(begin, 0, size - m_offset);
        if (end == nullptr)
        {
            m_ok = false;
            std::string msg(__FUNCTION_NAME__);
            msg.append(": terminating zero not found");
            throw container_overflow_exception(std::move(msg));
        }

        arg.assign(begin, end - begin);
        m_offset += arg.length() + 1;

        return *this;
    }

    in_message& in_message::operator >> (std::vector<uint8_t>& blob)
    {
        check_status<bad_message_exception>(m_ok, std::string(__FUNCTION_NAME__) + ": fail flag is set");

        __MSG_LENGTH_TYPE__ size = *(__MSG_LENGTH_TYPE__*)m_buffer.data();
#if __MSG_USE_TAGS__
        const size_t delta = 1 + sizeof(__MSG_LENGTH_TYPE__);
#else
        const size_t delta = sizeof(__MSG_LENGTH_TYPE__);
#endif // __MSG_USE_TAGS__
        if (size < m_offset + delta)
            fail_status(throw_message_too_short_exception, m_ok, __FUNCTION_NAME__, m_offset + delta, size);

#if __MSG_USE_TAGS__
        type_tag tag = (type_tag)m_buffer[m_offset];
        if (tag != type_tag::blob)
            fail_status(throw_type_mismatch_exception, m_ok, __FUNCTION_NAME__, to_string(tag), to_string(type_tag::blob));

        ++m_offset;
#endif // __MSG_USE_TAGS__

        const __MSG_LENGTH_TYPE__ blob_len = *(const __MSG_LENGTH_TYPE__*)&m_buffer[m_offset];
        m_offset += sizeof(__MSG_LENGTH_TYPE__);

        if (size < m_offset + blob_len)
            fail_status(throw_message_too_short_exception, m_ok, __FUNCTION_NAME__, m_offset + blob_len, size);

        if (blob_len != 0)
        {
            blob.resize(blob_len);
            memcpy(blob.data(), &m_buffer[m_offset], blob_len);
            m_offset += blob_len;
        }

        return *this;
    }
}
