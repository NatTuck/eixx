//----------------------------------------------------------------------------
/// \file connection_tcp.ipp
//----------------------------------------------------------------------------
/// \brief Implementation of TCP connectivity transport with an Erlang node.
//----------------------------------------------------------------------------
// Copyright (c) 2010 Serge Aleynikov <saleyn@gmail.com>
// Created: 2010-09-11
//----------------------------------------------------------------------------
/*
***** BEGIN LICENSE BLOCK *****

This file is part of the eixx (Erlang C++ Interface) library.

Copyright (c) 2010 Serge Aleynikov <saleyn@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

***** END LICENSE BLOCK *****
*/

#ifndef _EIXX_CONNECTION_TCP_IPP_
#define _EIXX_CONNECTION_TCP_IPP_

extern "C" {

#include <misc/eimd5.h>             // see erl_interface/src
#include <epmd/ei_epmd.h>           // see erl_interface/src
#include <connect/ei_connect_int.h> // see erl_interface/src

}

namespace EIXX_NAMESPACE {
namespace connect {

//------------------------------------------------------------------------------
// connection_tcp implementation
//------------------------------------------------------------------------------

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::start() 
{
    boost::asio::ip::tcp::socket::non_blocking_io nb(true);
    m_socket.io_control(nb);

    base_t::start(); // trigger on_connect callback
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::connect(
    const std::string& a_this_node, const std::string& a_remote_node, const std::string& a_cookie) 
    throw(std::runtime_error)
{
    using boost::asio::ip::tcp;

    base_t::connect(a_this_node, a_remote_node, a_cookie);

    if (a_this_node.find('@', 0) == std::string::npos)
        THROW_RUNTIME_ERROR("Invalid format of this_node: " << a_this_node);
    if (a_remote_node.find('@', 0) == std::string::npos)
        THROW_RUNTIME_ERROR("Invalid format of remote_node: " << a_remote_node);

    boost::system::error_code err = boost::asio::error::host_not_found;
    std::stringstream es;

    m_socket.close();

    // First resolve remote host name and connect to EPMD to find out the
    // node's port number.

    const char* epmd_port_s = getenv("ERL_EPMD_PORT");
    std::stringstream str;
    str << EPMD_PORT;

    const char* epmd_port = (epmd_port_s != NULL) ? epmd_port_s : str.str().c_str();

    tcp::resolver::query q(remote_hostname(), epmd_port);
    m_state = CS_WAIT_RESOLVE;
    m_resolver.async_resolve(q,
        boost::bind(&tcp_connection<Handler, Alloc>::handle_resolve, shared_from_this(),
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::iterator));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_resolve(
    const boost::system::error_code& err, boost::asio::ip::tcp::resolver::iterator ep_iterator)
{
    BOOST_ASSERT(m_state == CS_WAIT_RESOLVE);
    if (err) {
        std::stringstream str; str << "Error resolving address of node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        return;
    }
    // Attempt a connection to the first endpoint in the list. Each endpoint
    // will be tried until we successfully establish a connection.
    m_peer_endpoint = *ep_iterator;
    m_state = CS_WAIT_EPMD_CONNECT;
    m_socket.async_connect(m_peer_endpoint,
        boost::bind(&tcp_connection<Handler, Alloc>::handle_epmd_connect, shared_from_this(),
            boost::asio::placeholders::error, ++ep_iterator));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_epmd_connect(
    const boost::system::error_code& err, boost::asio::ip::tcp::resolver::iterator ep_iterator)
{
    BOOST_ASSERT(m_state == CS_WAIT_EPMD_CONNECT);
    if (!err) {
        // The connection was successful. Send the request.
        std::string alivename = remote_alivename();
        char* w = m_buf_epmd;
        int len = alivename.size() + 1;
        put16be(w,len);
        put8(w,EI_EPMD_PORT2_REQ);
        strcpy(w, alivename.c_str());

        if (this->handler()->verbose() >= VERBOSE_TRACE)
            std::cout << "-> sending epmd port req for '" << alivename << "': "
                      << to_binary_string(m_buf_epmd, len+2) << std::endl;

        m_state = CS_WAIT_EPMD_WRITE_DONE;
        boost::asio::async_write(m_socket, boost::asio::buffer(m_buf_epmd, len+2),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_epmd_write, shared_from_this(),
                        boost::asio::placeholders::error));
    } else if (ep_iterator != boost::asio::ip::tcp::resolver::iterator()) {
        // The connection failed. Try the next endpoint in the list.
        m_socket.close();
        m_peer_endpoint = *ep_iterator;
        m_socket.async_connect(m_peer_endpoint,
            boost::bind(&tcp_connection<Handler, Alloc>::handle_epmd_connect, shared_from_this(),
                boost::asio::placeholders::error, ++ep_iterator));
    } else {
        std::stringstream str; str << "Error connecting to epmd at host '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
    }
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_epmd_write(const boost::system::error_code& err)
{
    BOOST_ASSERT(m_state == CS_WAIT_EPMD_WRITE_DONE);
    if (err) {
        std::stringstream str; str << "Error writing to epmd at host '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }
        
    m_state = CS_WAIT_EPMD_REPLY;
    m_epmd_wr = m_buf_epmd;
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buf_epmd, sizeof(m_buf_epmd)),
        boost::asio::transfer_at_least(2),
        boost::bind(&tcp_connection<Handler, Alloc>::handle_epmd_read_header, shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_epmd_read_header(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_EPMD_REPLY);
    if (err) {
        std::stringstream str; str << "Error reading response from epmd at host '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    const char* s = m_buf_epmd;
    int res = get8(s);

    if (res != EI_EPMD_PORT2_RESP) { // response type
        std::stringstream str; str << "Error unknown response from epmd at host '" 
            << this->remote_node() << "': " << res;
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    int n = get8(s);

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "<- response from epmd: " << n 
                  << " (" << (n ? "failed" : "ok") << ')' << std::endl;

    if (n) { // Got negative response
        std::stringstream str; str << "Node " << this->remote_node() << " not known to epmd!";
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_epmd_wr = m_buf_epmd + bytes_transferred;

    int need_bytes = 8 - (bytes_transferred - 2);
    if (need_bytes > 0) {
        boost::asio::async_read(m_socket, 
            boost::asio::buffer(m_epmd_wr, sizeof(m_buf_epmd)-bytes_transferred),
            boost::asio::transfer_at_least(need_bytes),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_epmd_read_body, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
    } else {
        handle_epmd_read_body(boost::system::error_code(), 0);
    }
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_epmd_read_body(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_EPMD_REPLY);
    if (err) {
        std::stringstream str; str << "Error reading response body from epmd at host '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_epmd_wr += bytes_transferred;
    int got_bytes = m_epmd_wr - m_buf_epmd;
    BOOST_ASSERT(got_bytes >= 10);

    const char* s      = m_buf_epmd + 2;
    int port           = get16be(s);
    int ntype          = get8(s);
    int proto          = get8(s);
    uint16_t dist_high = get16be(s);
    uint16_t dist_low  = get16be(s);
    m_dist_version     = (dist_high > EI_DIST_HIGH ? EI_DIST_HIGH : dist_high);

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "<- epmd returned: port=" << port << ",ntype=" << ntype
                  << ",proto=" << proto << ",dist_high=" << dist_high
                  << ",dist_low=" << dist_low << std::endl;
 
    m_peer_endpoint.port(port);
    m_peer_endpoint.address( m_socket.remote_endpoint().address() );
    m_socket.close();

    if (m_dist_version <= 4) {
        std::stringstream str; str << "Incompatible version " << m_dist_version
            << " of remote node '" << this->remote_node() << "'";
        this->handler()->on_connect_failure(this, str.str());
        return;
    }

    // Change the endpoint's port to the one resolved from epmd
    m_state = CS_WAIT_CONNECT;

    m_socket.async_connect(m_peer_endpoint,
        boost::bind(&tcp_connection<Handler, Alloc>::handle_connect, shared_from_this(),
            boost::asio::placeholders::error));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_connect(const boost::system::error_code& err)
{
    BOOST_ASSERT(m_state == CS_WAIT_CONNECT);
    if (err) {
        std::stringstream str; str << "Cannot connect to node " << this->remote_node() 
            << " at port " << m_peer_endpoint.port() << ": " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "<- Connected to node: " << this->remote_node() << std::endl;

    m_our_challenge = gen_challenge();

    // send challenge
    int siz = 2 + 1 + 2 + 4 + this->m_this_node.size();

    if (siz > sizeof(m_buf_node)) {
        std::stringstream str; str << "Node name too long: " << this->this_node() 
            << " [" << __FILE__ << ':' << __LINE__ << ']';
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    char* w = m_buf_node;
    put16be(w, siz - 2);
    put8(w, 'n');
    put16be(w, m_dist_version);
    put32be(w,  ( DFLAG_EXTENDED_REFERENCES
                | DFLAG_EXTENDED_PIDS_PORTS
                | DFLAG_FUN_TAGS
                | DFLAG_NEW_FUN_TAGS
                | DFLAG_NEW_FLOATS
                | DFLAG_DIST_MONITOR));
    memcpy(w, this->this_node().c_str(), this->this_node().size());

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "-> sending name " << siz << " bytes:" 
                  << to_binary_string(m_buf_node, siz) << std::endl;

    m_state = CS_WAIT_WRITE_CHALLENGE_DONE;
    boost::asio::async_write(m_socket, boost::asio::buffer(m_buf_node, siz),
        boost::bind(&tcp_connection<Handler, Alloc>::handle_write_name, shared_from_this(),
                    boost::asio::placeholders::error));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_write_name(const boost::system::error_code& err)
{
    BOOST_ASSERT(m_state == CS_WAIT_WRITE_CHALLENGE_DONE);
    if (err) {
        std::stringstream str; str << "Error writing auth challenge to node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }
    m_state = CS_WAIT_STATUS;
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buf_node, sizeof(m_buf_node)),
        boost::asio::transfer_at_least(2),
        boost::bind(&tcp_connection<Handler, Alloc>::handle_read_status_header, shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_status_header(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_STATUS);
    if (err) {
        std::stringstream str; str << "Error reading auth status from node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_rd = m_buf_node;
    size_t len = get16be(m_node_rd);

    if (len != 3) {
        std::stringstream str; str << "Node " << this->remote_node() 
            << " rejected connection with reason: " << std::string(m_node_rd, len);
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr = m_buf_node + bytes_transferred;

    int need_bytes = 5 - (bytes_transferred - 2);
    if (need_bytes > 0) {
        boost::asio::async_read(m_socket, 
            boost::asio::buffer(m_node_wr, (m_buf_node+1) - m_node_wr),
            boost::asio::transfer_at_least(need_bytes),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_read_status_body, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
    } else {
        handle_read_status_body(boost::system::error_code(), 0);
    }
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_status_body(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_STATUS);
    if (err) {
        std::stringstream str; str << "Error reading auth status body from node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr += bytes_transferred;
    int got_bytes = m_node_wr - m_node_rd;
    BOOST_ASSERT(got_bytes >= 3);

    if (memcmp(m_node_rd, "sok", 3) != 0) {
        std::stringstream str; str << "Error invalid auth status response '" 
            << this->remote_node() << "': " << std::string(m_node_rd, 3);
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_state = CS_WAIT_CHALLENGE;

    // recv challenge
    m_node_rd += 3;
    got_bytes -= 3;
    if (got_bytes >= 2)
        handle_read_challenge_header(boost::system::error_code(), 0);
    else
        boost::asio::async_read(m_socket, boost::asio::buffer(m_node_wr, (m_buf_node+1) - m_node_wr),
            boost::asio::transfer_at_least(2),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_read_challenge_header, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_challenge_header(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_CHALLENGE);
    if (err) {
        std::stringstream str; str << "Error reading auth challenge from node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr += bytes_transferred;
    m_expect_size = get16be(m_node_rd);

    if ((m_expect_size - 11) > MAXNODELEN || m_expect_size > (m_buf_node+1)-m_node_rd) {
        std::stringstream str; str << "Error in auth status challenge node length " 
            << this->remote_node() << " : " << m_expect_size;
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    int need_bytes = m_expect_size - (m_node_wr - m_node_rd);
    if (need_bytes > 0) {
        boost::asio::async_read(m_socket, 
            boost::asio::buffer(m_node_wr, (m_buf_node+1) - m_node_wr),
            boost::asio::transfer_at_least(need_bytes),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_read_challenge_body, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
    } else {
        handle_read_challenge_body(boost::system::error_code(), 0);
    }
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_challenge_body(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_CHALLENGE);
    if (err) {
        std::stringstream str; str << "Error reading auth challenge body from node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr += bytes_transferred;
    int got_bytes = m_node_wr - m_node_rd;
    BOOST_ASSERT(got_bytes >= (int)m_expect_size);

    char tag = get8(m_node_rd);
    if (tag != 'n') {
        std::stringstream str; str << "Error reading auth challenge tag '" 
            << this->remote_node() << "': " << tag;
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    int version        = get16be(m_node_rd);
    int flags          = get32be(m_node_rd);
    m_remote_challenge = get32be(m_node_rd);

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "<- got auth challenge (version=" << version 
                  << ", flags=" << flags << ", remote_challenge=" << m_remote_challenge
                  << ')' << std::endl;

    uint8_t our_digest[16];
    gen_digest(m_remote_challenge, this->m_cookie.c_str(), our_digest);

    // send challenge reply
    int siz = 2 + 1 + 4 + 16;
    char* w = m_buf_node;
    put16be(w,siz - 2);
    put8(w, 'r');
    put32be(w, m_our_challenge);
    memcpy (w, our_digest, 16);

    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "-> sending challenge reply " << siz << " bytes:"
                  << to_binary_string(m_buf_node, siz) << std::endl;

    m_state = CS_WAIT_WRITE_CHALLENGE_REPLY_DONE;
    boost::asio::async_write(m_socket, boost::asio::buffer(m_buf_node, siz),
        boost::bind(&tcp_connection<Handler, Alloc>::handle_write_challenge_reply, shared_from_this(),
                    boost::asio::placeholders::error));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_write_challenge_reply(const boost::system::error_code& err)
{
    BOOST_ASSERT(m_state == CS_WAIT_WRITE_CHALLENGE_REPLY_DONE);
    if (err) {
        std::stringstream str; str << "Error writing auth challenge reply to node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }
    m_state = CS_WAIT_CHALLENGE_ACK;
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buf_node, sizeof(m_buf_node)),
        boost::asio::transfer_at_least(2),
        boost::bind(&tcp_connection<Handler, Alloc>::handle_read_challenge_ack_header, shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_challenge_ack_header(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_CHALLENGE_ACK);
    if (err) {
        std::stringstream str; str << "Error reading auth challenge ack from node '" 
            << this->remote_node() << "': "
            << (err == boost::asio::error::eof ? "Possibly bad cookie?" : err.message());
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_rd = m_buf_node;
    m_expect_size = get16be(m_node_rd);

    if (m_expect_size > sizeof(m_buf_node)-2) {
        std::stringstream str; str << "Error in auth status challenge ack length " 
            << this->remote_node() << " : " << m_expect_size;
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr = m_buf_node + bytes_transferred;

    int need_bytes = m_expect_size - (m_node_wr - m_node_rd);
    if (need_bytes > 0) {
        boost::asio::async_read(m_socket, 
            boost::asio::buffer(m_node_wr, (m_buf_node+1)-m_node_wr),
            boost::asio::transfer_at_least(need_bytes),
            boost::bind(&tcp_connection<Handler, Alloc>::handle_read_challenge_ack_body, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
    } else {
        handle_read_challenge_ack_body(boost::system::error_code(), 0);
    }
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::handle_read_challenge_ack_body(
    const boost::system::error_code& err, size_t bytes_transferred)
{
    BOOST_ASSERT(m_state == CS_WAIT_CHALLENGE_ACK);
    if (err) {
        std::stringstream str; str << "Error reading auth challenge ack body from node '" 
            << this->remote_node() << "': " << err.message();
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_node_wr += bytes_transferred;
    int got_bytes = m_node_wr - m_node_rd;
    BOOST_ASSERT(got_bytes >= (int)m_expect_size);

    char tag = get8(m_node_rd);
    if (this->handler()->verbose() >= VERBOSE_TRACE)
        std::cout << "<- got auth challenge ack (tag=" << tag << "): " 
                  << to_binary_string(m_node_rd, m_expect_size-1) << std::endl;

    if (tag != 'a') {
        std::stringstream str; str << "Error reading auth challenge ack body tag '" 
            << this->remote_node() << "': " << tag;
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    char her_digest[16], expected_digest[16];
    memcpy(her_digest, m_node_rd, 16);
    gen_digest(m_our_challenge, this->m_cookie.c_str(), (uint8_t*)expected_digest);
    if (memcmp(her_digest, expected_digest, 16) != 0) {
        std::stringstream str; str << "Authentication failure at node '" 
            << this->remote_node() << '!';
        this->handler()->on_connect_failure(this, str.str());
        m_socket.close();
        return;
    }

    m_socket.set_option(boost::asio::ip::tcp::no_delay(true));
    m_socket.set_option(boost::asio::socket_base::keep_alive(true));

    m_state = CS_CONNECTED;

    this->start();
}

template <class Handler, class Alloc>
uint32_t tcp_connection<Handler, Alloc>::gen_challenge(void)
{
#if defined(__WIN32__)
    struct {
        SYSTEMTIME tv;
        DWORD cpu;
        int pid;
    } s;
    GetSystemTime(&s.tv);
    s.cpu  = GetTickCount();
    s.pid  = getpid();
    
#else
    struct { 
        struct timeval tv; 
        clock_t cpu; 
        pid_t pid; 
        u_long hid; 
        uid_t uid; 
        gid_t gid; 
        struct utsname name; 
    } s; 

    gettimeofday(&s.tv, 0); 
    uname(&s.name); 
    s.cpu  = clock(); 
    s.pid  = getpid(); 
    s.hid  = gethostid(); 
    s.uid  = getuid(); 
    s.gid  = getgid(); 
#endif
    return md_32((char*) &s, sizeof(s));
}

template <class Handler, class Alloc>
uint32_t tcp_connection<Handler, Alloc>::
md_32(char* string, int length)
{
    MD5_CTX ctx;
    union {
        char c[16];
        unsigned x[4];
    } digest;
    ei_MD5Init(&ctx);
    ei_MD5Update(&ctx, (unsigned char *) string, (unsigned) length);
    ei_MD5Final((unsigned char *) digest.c, &ctx);
    return (digest.x[0] ^ digest.x[1] ^ digest.x[2] ^ digest.x[3]);
}

template <class Handler, class Alloc>
void tcp_connection<Handler, Alloc>::
gen_digest(unsigned challenge, const char cookie[], uint8_t digest[16])
{
    MD5_CTX c;
    char chbuf[21];
    sprintf(chbuf,"%u", challenge);
    ei_MD5Init(&c);
    ei_MD5Update(&c, (unsigned char *) cookie, (uint32_t) strlen(cookie));
    ei_MD5Update(&c, (unsigned char *) chbuf,  (uint32_t) strlen(chbuf));
    ei_MD5Final(digest, &c);
}

} // namespace connect
} // namespace EIXX_NAMESPACE

#endif // _EIXX_CONNECTION_TCP_IPP_

