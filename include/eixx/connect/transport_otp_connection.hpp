//----------------------------------------------------------------------------
/// \file  transport_otp_connection.hpp
//----------------------------------------------------------------------------
/// \brief Unified connection class abstracting TCP/UDP/PIPE connection types.
//----------------------------------------------------------------------------
// Copyright (c) 2010 Serge Aleynikov <saleyn@gmail.com>
// Created: 2010-09-11
//----------------------------------------------------------------------------
/*
 * ***** BEGIN LICENSE BLOCK *****
 *
 * This file is part of the eixx (Erlang C++ Interface) library.
 *
 * Copyright (c) 2010 Serge Aleynikov <saleyn@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ***** END LICENSE BLOCK *****
 */

#ifndef _EIXX_TRANSPORT_OTP_CONNECTION_HPP_
#define _EIXX_TRANSPORT_OTP_CONNECTION_HPP_

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/algorithm/string.hpp>
#include <eixx/connect/common.hpp>
#include <eixx/connect/verbose.hpp>
#include <eixx/marshal/string.hpp>

namespace EIXX_NAMESPACE {
namespace connect {

namespace posix = boost::asio::posix;

/// Types of connections supported by this class.
enum connection_type { UNDEFINED, TCP, UDS };

/// Convert connection type to string.
const char* connection_type_to_str(connection_type a_type);

//----------------------------------------------------------------------------
// Base connection class.
//----------------------------------------------------------------------------

/// Represents a single connection to an Erlang node. The class abstracts 
/// underlying transport and is responsible for message delimination, parsing
/// and communication between the Handler and transport layer.
template <typename Handler, typename Alloc>
class connection
    : private boost::noncopyable
    , public boost::enable_shared_from_this< connection<Handler, Alloc> >
{
protected:
    static const size_t         s_header_size  = 4;
    static const char           s_header_magic = 132;
    static const eterm<Alloc>   s_null_cookie;

    boost::asio::io_service&    m_io_service;
    /// The handler used to process the incoming request.
    Handler*                    m_handler;
    connection_type             m_type;
    std::string                 m_remote_node;
    std::string                 m_this_node;
    std::string                 m_cookie;

    Alloc                       m_allocator;

    bool                        m_got_header;       /// true after packet header was read
    size_t                      m_packet_size;      /// size of next incoming packet
    size_t                      m_in_msg_count;
    size_t                      m_out_msg_count;

    std::vector<char, Alloc>    m_rd_buf;           /// buffer for incoming data
    char*                       m_rd_ptr;
    char*                       m_rd_end;

    std::deque<boost::asio::const_buffer> 
                                m_out_msg_queue[2]; /// Queues of outgoing data
                                                    /// First queue is used for cacheing messages
                                                    /// while the second queue is used for 
                                                    /// writing them to socket.
    size_t                      m_available_queue;  /// Index of the queue used for cacheing
    bool                        m_is_writing;
    bool                        m_connection_aborted;

    /// Construct a connection
    connection(connection_type a_ct, boost::asio::io_service& a_svc, 
               Handler* a_h, const Alloc& a_alloc)
        : m_io_service(a_svc), m_handler(a_h), m_type(a_ct)
        , m_allocator(a_alloc)
        , m_got_header(false), m_packet_size(s_header_size)
        , m_in_msg_count(0), m_out_msg_count(0)
        , m_rd_buf(4096), m_rd_ptr(&*m_rd_buf.begin()), m_rd_end(&*m_rd_buf.begin())
        , m_available_queue(0)
        , m_is_writing(false)
        , m_connection_aborted(false)
    {
        if (handler()->verbose() >= VERBOSE_TRACE)
            std::cout << "Calling connection::connection(type=" << m_type << ')' << std::endl;
    }

    char* allocate(size_t a_sz)    {
        char* p = m_allocator.allocate(a_sz+1);
        *p++ = s_header_magic;
        return p;
    }

    /// Swap available and writing queue indexes.
    void   flip_queues()              { m_available_queue = writing_queue(); }
    /// Index of the queue used for writing to socket
    size_t writing_queue()      const { return (m_available_queue+1) & 1; }
    /// Index of the queue used for cacheing data to be writen to socket.
    size_t available_queue()    const { return m_available_queue; }

    char*  rd_ptr()                 { return m_rd_ptr; }
    size_t rd_length()              { return m_rd_end - m_rd_ptr; }
    size_t rd_size()                { return m_rd_buf.size() - rd_length(); }
    /// Verboseness
    verbose_type verbose()    const { return m_handler->verbose(); }

    void do_write(boost::asio::const_buffer& a_buf) {
        m_out_msg_queue[available_queue()].push_back(a_buf);
        do_write_internal();
    }

    void do_write_internal() {
        if (!m_is_writing && !m_out_msg_queue[available_queue()].empty()) {
            typedef boost::asio::detail::consuming_buffers<
                boost::asio::const_buffer, 
                std::deque<boost::asio::const_buffer> 
            > cb_t;
            cb_t buffers(m_out_msg_queue[available_queue()]);
            m_is_writing = true;
            flip_queues(); // Work on the data accumulated in the available_queue.
            if (unlikely(verbose() >= VERBOSE_WIRE))
                for(cb_t::const_iterator it=buffers.begin(); it != buffers.end(); ++it)
                    std::cout << "  async_write " << boost::asio::buffer_size(*it) << " bytes: " 
                        << marshal::to_binary_string(boost::asio::buffer_cast<const char*>(*it),
                                                     boost::asio::buffer_size(*it)) << std::endl;
            async_write(buffers, boost::asio::transfer_all(), 
                boost::bind(&connection<Handler, Alloc>::handle_write, this->shared_from_this(),
                    boost::asio::placeholders::error));
        }
    }

    void handle_write(const boost::system::error_code& err);
    void handle_read (const boost::system::error_code& err, size_t bytes_transferred);

    /// Decode distributed Erlang message.  The message must be fully
    /// stored in \a mbuf.
    /// Note: TICK message is represented by msg type = 0, in this case \a a_cntrl_msg
    /// and \a a_msg are invalid.
    /// @return Control Message
    int transport_msg_decode(const char *mbuf, int len, transport_msg<Alloc>& a_tm)
        throw(err_decode_exception);

    void process_message(const char* a_buf, size_t a_size);

    bool check_connected(const eterm<Alloc>* a_msg) {
        if (likely(!m_connection_aborted))
            return true;

        ON_ERROR_CALLBACK(this, "Connection closed"
                                << (a_msg ? ", message dropped: " : "")
                                << (a_msg ? a_msg->to_string() : ""));
        return false;
    }

    /// Write a message asynchronously to the socket.
    /// @param <a_msg> is the eterm to be written.
    void async_write(const eterm<Alloc>& a_msg) {
        if (unlikely(!check_connected(&a_msg)))
            return;
        // Allocate storage for holding the packet
        size_t sz  = a_msg.encode_size(s_header_size, true);
        char* data = allocate(sz);
        // Encode the packet to the allocated buffer.
        a_msg.encode(data, sz, s_header_size, true);
        BOOST_ASSERT(*(data - 1) == s_header_magic);

        if (unlikely(verbose() >= VERBOSE_MESSAGE))
            std::cout << "client -> agent: " << a_msg.to_string() << std::endl;
        if (unlikely(verbose() >= VERBOSE_WIRE))
            marshal::to_binary_string(std::cout << "client -> agent: ", data, sz) << std::endl;

        boost::asio::const_buffer b(data, sz);
        m_io_service.post(
            boost::bind(&connection<Handler, Alloc>::do_write, this->shared_from_this(), b));
    }

    /// Get connection type from string. If successful the string is 
    /// modified to exclude the "...://" prefix.
    /// @param <s> is a connection address (e.g. "tcp://node@host").
    /// @return connection type derived from <tt>s</tt>.
    static connection_type parse_connection_type(std::string& s) 
        throw(std::runtime_error);

    /// Establish connection to \a a_remote_node. The call is non-blocking -
    /// it will immediately returned, and Handler's on_connect() or 
    /// on_error() callback will be invoked on successful/failed connection
    /// status.
    virtual void connect(const std::string& a_this_node, 
                         const std::string& a_remote_node,
                         const std::string& a_cookie)
        throw(std::runtime_error)
    {
        m_this_node   = a_this_node;
        m_remote_node = a_remote_node;
        m_cookie      = a_cookie;
    }

    /// Set the socket to non-blocking mode and issue on_connect() callback.
    ///
    /// When implementing a server this method is to be called after 
    /// accepting a new connection.  When implementing a client, call
    /// connect() method instead, which invokes start() automatically. 
    virtual void start() {
        if (m_connection_aborted)
            return;

        if (handler()->verbose() >= VERBOSE_TRACE)
            std::cout << "Calling connection::start()" << std::endl;
        m_connection_aborted = false;
        m_handler->on_connect(this);

        const boost::asio::mutable_buffers_1 buffers(m_rd_end, rd_size());
        async_read(
            buffers,
            boost::asio::transfer_at_least(s_header_size),
            boost::bind(&connection<Handler, Alloc>::handle_read, this->shared_from_this(), 
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
    }

    template <class MutableBuffers, class CompletionCondition, class ReadHandler>
    void async_read(const MutableBuffers& b, const CompletionCondition& c, ReadHandler h);

    template <class MutableBuffers, class CompletionCondition, class ReadHandler>
    void async_write(const MutableBuffers& b, const CompletionCondition& c, ReadHandler h);

public:
    typedef Handler handler_type;
    typedef boost::shared_ptr<connection<Handler, Alloc> > pointer;

    /// Create a connection object given of specific type and connect to peer
    /// endpoint given by \a a_addr.
    /// @param a_svc is the service object.
    /// @param a_h is the callback handler object.
    /// @param a_node remote node name.
    /// The string can contain a URL udentifier in the following formats:
    /// \verbatim
    /// Examples:  node@google.com
    ///            abc@host
    ///            tcp://node@host
    ///            uds:///tmp/node_node  \endverbatim
    /// \endverbatim
    /// @param a_cookie security cookie.
    static pointer create(
        boost::asio::io_service& a_svc,
        handler_type*      a_h,
        const std::string& a_this_node,
        const std::string& a_node,
        const std::string& a_cookie,
        const Alloc&       a_alloc = Alloc());

    virtual ~connection() {
        if (handler()->verbose() >= VERBOSE_TRACE)
            std::cout << "Calling connection::~connection()" << std::endl;
    }

    /// Close connection channel orderly by user. 
    ///
    /// This method needs to be invoked by user when the socket needs
    /// to be disconnected orderly.  <tt>boost::asio::error::connection_aborted</tt>
    /// error is reported in the on_disconnect() callback.
    virtual void stop() {
        boost::system::error_code ec = boost::asio::error::connection_aborted;
        this->stop(ec);
    }

    /// Stop all asynchronous operations associated with the connection.
    ///
    /// This method needs to be invoked when there's a read/write error
    /// on the socket.
    /// @param e is the disconnect reason.
    virtual void stop(const boost::system::error_code& e) {
        if (m_connection_aborted)
            return;

        if (handler()->verbose() >= VERBOSE_TRACE)
            std::cout << "Calling connection::stop(" << e.message() << ')' << std::endl;

        m_connection_aborted = true;
        m_handler->on_disconnect(this, e);
        //delete this;
    }

    virtual int native_socket() = 0;

    /// Address of connected peer.
    virtual std::string         peer_address() const    { return ""; }
    const   std::string&        remote_node()  const    { return m_remote_node; }
    const   std::string&        this_node()    const    { return m_this_node; }
    const   std::string&        cookie()       const    { return m_cookie; }
    Handler*                    handler()               { return m_handler; }
    boost::asio::io_service&    io_service()            { return m_io_service; }

    /// Send a message \a a_msg to the remote node.
    void send(const transport_msg<Alloc>& a_msg);

    void on_error(const std::string& s) {
        m_handler->on_error(this,  s);
    }
};

//------------------------------------------------------------------------------
// connection template implementation
//------------------------------------------------------------------------------

} // namespace connect
} // namespace EIXX_NAMESPACE

#include <eixx/connect/transport_otp_connection.ipp>

#endif // _EIXX_TRANSPORT_OTP_CONNECTION_HPP_
