/*
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef INCL_TRANSPORTAGENT
#define INCL_TRANSPORTAGENT

#include <string>
#include "SyncEvolutionUtil.h"

namespace SyncEvolution {

/**
 * Abstract API for a message send/receive agent.
 *
 * The calling sequence is as follows:
 * - set parameters for next message
 * - start message send
 * - optional: cancel transmission
 * - wait for completion and reply
 *
 * Data to be sent is owned by caller. Data received as reply is
 * allocated and owned by agent. Errors are reported via
 * TransportAgentException.
 */
class TransportAgent
{
 public:
     typedef bool (*TransportCallback) (void *udata);
    /**
     * set transport specific URL of next message
     */
    virtual void setURL(const std::string &url) = 0;

    /**
     * set proxy for transport, in protocol://[user@]host[:port] format
     */
    virtual void setProxy(const std::string &proxy) = 0;

    /**
     * set proxy user name (if not specified in proxy string)
     * and password
     */
    virtual void setProxyAuth(const std::string &user,
                              const std::string &password) = 0;

    /**
     * control how SSL certificates are checked
     *
     * @param cacerts         path to a single CA certificate file
     * @param verifyServer    enable server verification (should always be on)
     * @param verifyHost      do strict hostname checking in the certificate
     */
    virtual void setSSL(const std::string &cacerts,
                        bool verifyServer,
                        bool verifyHost) = 0;

    /**
     * define content type for post, see content type constants
     */
    virtual void setContentType(const std::string &type) = 0;

    /**
     * override default user agent string
     */
    virtual void setUserAgent(const std::string &agent) = 0;

    /**
     * start sending message
     *
     * Memory must remain valid until reply is received or
     * message transmission is canceled.
     *
     * @param data      start address of data to send
     * @param len       number of bytes
     */
    virtual void send(const char *data, size_t len) = 0;

    /**
     * cancel an active message transmission
     *
     * Blocks until send buffer is no longer in use.
     * Returns immediately if nothing pending.
     */
    virtual void cancel() = 0;

    enum Status {
        /**
         * message is being sent or reply received,
         * check again with wait()
         */
        ACTIVE,
        /**
         * received and buffered complete reply,
         * get acces to it with getReponse()
         */
        GOT_REPLY,
        /**
         * message wasn't sent, try again with send()
         */
        CANCELED,
        /**
         * sending message has failed
         */
        FAILED,
        /**
         * transport timeout
         */
        TIME_OUT,
        /**
         * unused transport, configure and use send()
         */
        INACTIVE
    };

    /**
     * wait for reply
     *
     * Returns immediately if no transmission is pending.
     */
    virtual Status wait() = 0;

    /**
     * The callback is called every interval seconds, with udata as the last
     * parameter. The callback will return true to indicate retry and false 
     * to indicate abort.
     */
    virtual void setCallback (TransportCallback cb, void * udata, int interval) = 0;

    /**
     * provides access to reply data
     *
     * Memory pointer remains valid as long as
     * transport agent is not deleted and no other
     * message is sent.
     */
    virtual void getReply(const char *&data, size_t &len, std::string &contentType) = 0;

    /** SyncML in XML format */
    static const char * const m_contentTypeSyncML;

    /** SyncML in WBXML format */
    static const char * const m_contentTypeSyncWBXML;

    /** normal HTTP URL encoded */
    static const char * const m_contentTypeURLEncoded;

};

class TransportException : public SyncEvolutionException
{
 public:
    TransportException(const std::string &file,
                       int line,
                       const std::string &what) :
    SyncEvolutionException(file, line, what) {}
    ~TransportException() throw() {}
};

} // namespace SyncEvolution

#endif // INCL_TRANSPORTAGENT
