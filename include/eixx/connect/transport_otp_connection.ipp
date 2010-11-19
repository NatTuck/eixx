//----------------------------------------------------------------------------
/// \file transport_otp_connection.ipp
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

#ifndef _EIXX_TRANSPORT_OTP_CONNECTION_IPP_
#define _EIXX_TRANSPORT_OTP_CONNECTION_IPP_

#include <eixx/connect/transport_otp_connection_tcp.hpp>
#include <eixx/connect/transport_otp_connection_uds.hpp>
#include <eixx/marshal/trace.hpp>
#include <misc/eiext.h>                 // see erl_interface/src

namespace EIXX_NAMESPACE {
namespace connect {

using EIXX_NAMESPACE::marshal::trace;

template <class Handler, class Alloc>
typename connection<Handler, Alloc>::pointer
connection<Handler, Alloc>::create(
    boost::asio::io_service& a_svc,
    Handler*            a_h,
    const std::string&  a_this_node,
    const std::string&  a_node,
    const std::string&  a_cookie,
    const Alloc&        a_alloc) 
{
    if (a_this_node.find('@') == std::string::npos)
        THROW_RUNTIME_ERROR("Invalid name of this node: " << a_this_node);

    std::string addr(a_node);

    connection_type con_type = parse_connection_type(addr);

    switch (con_type) {
        case TCP:
            if (addr.find('@') == std::string::npos)
                THROW_RUNTIME_ERROR("Invalid node name " << a_node);
            break;
        case UDS:
            if (addr.find_last_of('/') == std::string::npos)
                THROW_RUNTIME_ERROR("Invalid node name " << a_node);
            break;
        default:
            THROW_RUNTIME_ERROR("Invalid node transport type: " << a_node);
    }

    pointer p;
    switch (con_type) {
        case TCP:  p.reset(new tcp_connection<Handler, Alloc>(a_svc, a_h, a_alloc));  break;
        case UDS:  p.reset(new uds_connection<Handler, Alloc>(a_svc, a_h, a_alloc));  break;
        default:   THROW_RUNTIME_ERROR("Not implemented! (proto=" << con_type << ')');
    }

    p->connect(a_this_node, addr, a_cookie);
    return p;
}

template <class Handler, class Alloc>
connection_type 
connection<Handler, Alloc>::parse_connection_type(std::string& s) throw(std::runtime_error)
{
    size_t pos = s.find("://", 0);
    if (pos == std::string::npos)
        return TCP;
    std::string a = s.substr(0, pos);
    const char* types[] = {"UNDEFINED", "tcp", "uds"};
    for (size_t i=1; i < sizeof(types)/sizeof(char*); ++i)
        if (boost::iequals(a, types[i])) {
            s = s.substr(pos+1);
            return static_cast<connection_type>(i);
        }
    THROW_RUNTIME_ERROR("Unknown connection type: " << s);
}

// NOTE: This is somewhat ugly because addition of new connection types
// requires modification of async_read() and async_write() functions by 
// adding case statements.  However, there doesn't seem to be a way of 
// handling it more generically because the socket type is statically
// defined in ASIO and boost::asio::async_read()/async_write() funcations
// are template specialized for each socket type. Consequently we
// resolve invocation of the right async function by using a switch statement.

template <class Handler, class Alloc>
template <class MutableBuffers, class CompletionCondition, class ReadHandler>
void connection<Handler, Alloc>::async_read(
    const MutableBuffers& b, const CompletionCondition& c, ReadHandler h)
{
    switch (m_type) {
        case TCP: {
            boost::asio::ip::tcp::socket& s = 
                reinterpret_cast<tcp_connection<Handler, Alloc>*>(this)->socket();
            boost::asio::async_read(s, b, c, h);
            break;
        }
        case UDS: {
            boost::asio::local::stream_protocol::socket& s = 
                reinterpret_cast<uds_connection<Handler, Alloc>*>(this)->socket();
            boost::asio::async_read(s, b, c, h);
            break;
        }
        default:
            THROW_RUNTIME_ERROR("async_read: Not implemented! (type=" << m_type << ')');
    }
}

template <class Handler, class Alloc>
template <class MutableBuffers, class CompletionCondition, class ReadHandler>
void connection<Handler, Alloc>::async_write(
    const MutableBuffers& b, const CompletionCondition& c, ReadHandler h)
{
    switch (m_type) {
        case TCP: {
            boost::asio::ip::tcp::socket& s = 
                reinterpret_cast<tcp_connection<Handler, Alloc>*>(this)->socket();
            boost::asio::async_write(s, b, c, h);
            break;
        }
        case UDS: {
            boost::asio::local::stream_protocol::socket& s = 
                reinterpret_cast<uds_connection<Handler, Alloc>*>(this)->socket();
            boost::asio::async_write(s, b, c, h);
            break;
        }
        default:
            THROW_RUNTIME_ERROR("async_write: Not implemented! (type=" << m_type << ')');
    }
}

template <class Handler, class Alloc>
void connection<Handler, Alloc>::
handle_write(const boost::system::error_code& err)
{
    if (m_connection_aborted) {
        if (verbose() >= VERBOSE_TRACE)
            std::cout << "Connection aborted - "
                         "exiting connection<Handler, Alloc>::handle_write" 
                      << std::endl;
        return;
    }

    if (unlikely(err)) {
        if (verbose() >= VERBOSE_TRACE)
            std::cout << "connection<Handler, Alloc>::handle_write("
                      << err.value() << ')' << std::endl;
        // We use operation_aborted as a user-initiated connection reset,
        // therefore check to substitute the error since bytes_transferred == 0
        // means a connection loss.
        boost::system::error_code e = 
            err == boost::asio::error::operation_aborted
            ? boost::asio::error::not_connected : err;
        stop(e);
        return;
    }
    for (std::deque<boost::asio::const_buffer>::iterator 
                it  = m_out_msg_queue[writing_queue()].begin(),
                end = m_out_msg_queue[writing_queue()].end();
            it != end; ++it) {
        const char* p = boost::asio::buffer_cast<const char*>(*it);
        // Don't forget to adjust for the header magic byte.
        BOOST_ASSERT(*(p - 1) == s_header_magic);
        m_allocator.deallocate(
            const_cast<char*>(p-1), boost::asio::buffer_size(*it)+1);
    }
    m_out_msg_queue[writing_queue()].clear();
    m_is_writing = false;
    do_write_internal();
}

template <class Handler, class Alloc>
void connection<Handler, Alloc>::
handle_read(const boost::system::error_code& err, size_t bytes_transferred)
{
    if (unlikely(verbose() >= VERBOSE_TRACE))
        std::cout << "connection<Handler, Alloc>::handle_read(transferred=" 
                  << bytes_transferred << ", got_header=" 
                  << (m_got_header ? "true" : "false")
                  << ", pkt_sz=" << m_packet_size << ", ec="
                  << err.value() << ')' << std::endl;

    if (unlikely(m_connection_aborted)) {
        if (verbose() >= VERBOSE_TRACE)
            std::cout << "Connection aborted - "
                         "exiting connection<Handler, Alloc>::handle_read" 
                      << std::endl;
        return;
    } else if (unlikely(err)) {
        // We use operation_aborted as a user-initiated connection reset,
        // therefore check to substitute the error since bytes_transferred == 0
        // means a connection loss.
        boost::system::error_code e = 
            err == boost::asio::error::operation_aborted && bytes_transferred == 0
            ? boost::asio::error::not_connected : err;
        stop(e);
        return;
    } else if (m_got_header) {
        m_rd_end += bytes_transferred;
    } else {
        // Make sure that the buffer size is large enouch to store
        // next message.
        BOOST_ASSERT(bytes_transferred >= s_header_size);
        bytes_transferred -= s_header_size;
        m_packet_size = boost::detail::load_big_endian<uint32_t, s_header_size>(m_rd_ptr);
        if (m_packet_size > m_rd_buf.size()-s_header_size) {
            m_rd_buf.reserve(m_packet_size + s_header_size);
            m_rd_ptr = &*m_rd_buf.begin();
        }
        m_rd_ptr    += s_header_size;
        m_rd_end     = m_rd_ptr + bytes_transferred;
        m_got_header = true;
    }
    // Process all messages in the buffer
    while ((m_rd_ptr + m_packet_size) <= m_rd_end) {
        m_in_msg_count++;
        try {
            if (verbose() >= VERBOSE_WIRE)
                marshal::to_binary_string(
                    std::cout << "client <- agent: ", m_rd_ptr, m_packet_size) << std::endl;

            // Decode the packet into a message and dispatch it.
            process_message(m_rd_ptr, m_packet_size);

        } catch (std::exception& e) {
            ON_ERROR_CALLBACK(this,
                "Error processing packet from server: " << e.what() << std::endl << "  ";
                marshal::to_binary_string(m_rd_ptr, m_packet_size));
        }
        m_rd_ptr += m_packet_size;
        m_got_header = ((m_rd_end - m_rd_ptr) >= (long)s_header_size);
        if (!m_got_header)
            break;
        m_packet_size = boost::detail::load_big_endian<uint32_t, s_header_size>(m_rd_ptr);
        m_rd_ptr     += s_header_size;
    }
    if (m_rd_ptr == m_rd_end) {
        m_rd_ptr = &m_rd_buf[0];
        m_rd_end = m_rd_ptr;
        m_packet_size = s_header_size;
    } else {
        // Crunch the buffer by copying leftover bytes to the beginning of the buffer.
        const size_t len = m_rd_end - m_rd_ptr;
        char* begin = &m_rd_buf[0];
        memcpy(begin, m_rd_ptr, len);
        m_rd_ptr = begin;
        m_rd_end = begin + len;
    }

    if (unlikely(verbose() >= VERBOSE_TRACE))
        std::cout << "Scheduling connection::async_read(offset=" 
                  << (m_rd_end-&m_rd_buf[0])
                  << ", sz=" << rd_size() << ", transf_at_least=" 
                  << m_packet_size << ", got_header="
                  << (m_got_header ? "true" : "false")
                  << ", aborted=" << (m_connection_aborted ? "true" : "false") << ')'
                  << std::endl;

    boost::asio::mutable_buffers_1 buffers(m_rd_end, rd_size());
    async_read(
        buffers, boost::asio::transfer_at_least(m_packet_size),
        boost::bind(&connection<Handler, Alloc>::handle_read, this->shared_from_this(), 
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

/// Decode distributed Erlang message.  The message must be fully
/// stored in \a mbuf.
/// Note: TICK message is represented by msg type = 0, in this case \a a_cntrl_msg
/// and \a a_msg are invalid.
/// @return message type
template <class Handler, class Alloc>
int connection<Handler, Alloc>::
transport_msg_decode(const char *mbuf, int len, transport_msg<Alloc>& a_tm)
    throw(err_decode_exception)
{
    const char* s = mbuf;
    int version;
    int index = 0;

    if (unlikely(len == 0)) // This is TICK message
        return ERL_TICK;

    /* now decode header */
    /* pass-through, version, control tuple header, control message type */
    if (unlikely(get8(s) != ERL_PASS_THROUGH))
        throw err_decode_exception("Missing pass-throgh flag in message");

    if (unlikely(ei_decode_version(s,&index,&version)) || unlikely((version != ERL_VERSION_MAGIC)))
        throw err_decode_exception("Invalid control message magic number", version);

    tuple<Alloc> cntrl(s, index, len, m_allocator);

    int msgtype = cntrl[0].to_long();

    if (unlikely(msgtype <= ERL_TICK) || unlikely(msgtype > ERL_MONITOR_P_EXIT))
        throw err_decode_exception("Invalid message type", msgtype);

    static const uint32_t types_with_payload = 1 << ERL_SEND
                                             | 1 << ERL_REG_SEND
                                             | 1 << ERL_SEND_TT
                                             | 1 << ERL_REG_SEND_TT;
    if (likely((1 << msgtype) & types_with_payload)) {
        if (unlikely(ei_decode_version(s,&index,&version)) || unlikely((version != ERL_VERSION_MAGIC)))
            throw err_decode_exception("Invalid message magic number", version);

        eterm<Alloc> msg(s, index, len, m_allocator);
        a_tm.set(msgtype, cntrl, &msg);
    } else {
        a_tm.set(msgtype, cntrl);
    }
        
    return msgtype;
}

template <class Handler, class Alloc>
void connection<Handler, Alloc>::
process_message(const char* a_buf, size_t a_size)
{
    transport_msg<Alloc> tm;
    int msgtype = transport_msg_decode(a_buf, a_size, tm);

    switch (msgtype) {
        case ERL_TICK: {
            // Reply with TOCK packet
            char* data = allocate(s_header_size);
            bzero(data, s_header_size);
            boost::asio::const_buffer b(data, s_header_size);
            do_write(b);
            break;
        }
        /*
        case ERL_SEND:
        case ERL_REG_SEND:
        case ERL_LINK:
        case ERL_UNLINK:
        case ERL_GROUP_LEADER:
        case ERL_EXIT:
        case ERL_EXIT2:
        case ERL_NODE_LINK:
        case ERL_MONITOR_P:
        case ERL_DEMONITOR_P:
        case ERL_MONITOR_P_EXIT:
            ...
            break;
        */
        default:
            if (unlikely(verbose() >= VERBOSE_WIRE))
                std::cout << "Got transport msg - (cntrl): " << tm.cntrl() << std::endl;
            if (unlikely(verbose() >= VERBOSE_MESSAGE))
                if (tm.has_msg())
                    std::cout << "Got transport msg - (msg):   " << tm.msg() << std::endl;
            m_handler->on_message(this, tm);
    }
}

template <class Handler, class Alloc>
void connection<Handler, Alloc>::
send(const transport_msg<Alloc>& a_msg)
{
    if (!check_connected(&a_msg.msg()))
        return;

    eterm<Alloc> l_cntrl(a_msg.cntrl());
    bool   l_has_msg= a_msg.has_msg();
    size_t cntrl_sz = l_cntrl.encode_size(0, true);
    size_t msg_sz   = l_has_msg ? a_msg.msg().encode_size(0, true) : 0;
    size_t len      = cntrl_sz + msg_sz + 1 /*passthrough*/ + 1 /*version*/ + 4 /*len*/;
    char*  data     = allocate(len);
    char*  s        = data;
    int    idx      = 0;
    put32be(s, len-4);
    *s++ = ERL_PASS_THROUGH;
    l_cntrl.encode(s, cntrl_sz, 0, true);
    if (l_has_msg)
        a_msg.msg().encode(s + cntrl_sz, msg_sz, 0, true);

    if (unlikely(verbose() >= VERBOSE_MESSAGE))
        std::cout << "SEND cntrl="
                  << l_cntrl.to_string() << (l_has_msg ? ", msg=" : "")
                  << (l_has_msg ? a_msg.msg().to_string() : std::string("")) << std::endl;
    //if (unlikely(verbose() >= VERBOSE_WIRE))
    //    std::cout << "SEND " << len << " bytes " << marshal::to_binary_string(data, len) << std::endl;

    boost::asio::const_buffer b(data, len);
    m_io_service.post(
        boost::bind(&connection<Handler, Alloc>::do_write, this->shared_from_this(), b));
}

} // namespace connect
} // namespace EIXX_NAMESPACE

#endif // _EIXX_TRANSPORT_OTP_CONNECTION_IPP_

