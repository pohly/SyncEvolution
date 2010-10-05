/*
 * Copyright (C) 2010 Patrick Ohly <patrick.ohly@gmx.de>
 */

#include "NeonCXX.h"
#include <ne_socket.h>
#include <ne_auth.h>

#include <list>
#include <boost/algorithm/string/join.hpp>

#include <syncevo/TransportAgent.h>
#include <syncevo/util.h>
#include <syncevo/Logging.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

namespace Neon {
#if 0
}
#endif

std::string features()
{
    std::list<std::string> res;

    if (ne_has_support(NE_FEATURE_SSL)) { res.push_back("SSL"); }
    if (ne_has_support(NE_FEATURE_ZLIB)) { res.push_back("ZLIB"); }
    if (ne_has_support(NE_FEATURE_IPV6)) { res.push_back("IPV6"); }
    if (ne_has_support(NE_FEATURE_LFS)) { res.push_back("LFS"); }
    if (ne_has_support(NE_FEATURE_SOCKS)) { res.push_back("SOCKS"); }
    if (ne_has_support(NE_FEATURE_TS_SSL)) { res.push_back("TS_SSL"); }
    if (ne_has_support(NE_FEATURE_I18N)) { res.push_back("I18N"); }
    return boost::join(res, ", ");
}

URI URI::parse(const std::string &url)
{
    URI res;

    ne_uri uri;
    int error = ne_uri_parse(url.c_str(), &uri);
    if (uri.scheme) { res.m_scheme = uri.scheme; }
    if (uri.host) { res.m_host = uri.host; }
    if (uri.userinfo) { res.m_userinfo = uri.userinfo; }
    if (uri.path) { res.m_path = uri.path; }
    if (uri.query) { res.m_query = uri.query; }
    if (uri.fragment) { res.m_fragment = uri.fragment; }
    res.m_port = uri.port ?
        uri.port : ne_uri_defaultport(res.m_scheme.c_str());
    ne_uri_free(&uri);
    if (error) {
        SE_THROW_EXCEPTION(TransportException,
                           StringPrintf("invalid URL '%s' (parsed as '%s')",
                                        url.c_str(),
                                        res.toURL().c_str()));
    }
    return res;
}

std::string URI::toURL() const
{
    return StringPrintf("%s://%s@%s:%u/%s#%s",
                        m_scheme.c_str(),
                        m_userinfo.c_str(),
                        m_host.c_str(),
                        m_port,
                        m_path.c_str(),
                        m_fragment.c_str());
}

Session::Session(const boost::shared_ptr<Settings> &settings) :
    m_settings(settings),
    m_session(NULL)
    
{
    int logLevel = m_settings->logLevel();
    if (logLevel >= 3) {
        ne_debug_init(stderr,
                      NE_DBG_FLUSH|NE_DBG_HTTP|NE_DBG_HTTPAUTH|
                      (logLevel >= 4 ? NE_DBG_HTTPBODY : 0) |
                      (logLevel >= 5 ? (NE_DBG_XML|NE_DBG_LOCKS|NE_DBG_SSL) : 0)|
                      (logLevel >= 6 ? (NE_DBG_XMLPARSE) : 0)|
                      (logLevel >= 11 ? (NE_DBG_HTTPPLAIN) : 0));
    } else {
        ne_debug_init(NULL, 0);
    }

    ne_sock_init();
    m_uri = URI::parse(settings->getURL());
    m_session = ne_session_create(m_uri.m_scheme.c_str(),
                                  m_uri.m_host.c_str(),
                                  m_uri.m_port);
    ne_set_server_auth(m_session, getCredentials, this);
    ne_ssl_set_verify(m_session, sslVerify, this);
}

Session::~Session()
{
    if (m_session) {
        ne_session_destroy(m_session);
    }
    ne_sock_exit();
}

int Session::getCredentials(void *userdata, const char *realm, int attempt, char *username, char *password) throw()
{
    try {
        Session *session = static_cast<Session *>(userdata);
        std::string user, pw;
        session->m_settings->getCredentials(realm, user, pw);
        SyncEvo::Strncpy(username, user.c_str(), NE_ABUFSIZ);
        SyncEvo::Strncpy(password, pw.c_str(), NE_ABUFSIZ);
        // allow only one attempt, credentials are not expected to change in most cases
        return attempt;
    } catch (...) {
        Exception::handle();
        SE_LOG_ERROR(NULL, NULL, "no credentials for %s", realm);
        return 1;
    }
}

int Session::sslVerify(void *userdata, int failures, const ne_ssl_certificate *cert) throw()
{
    try {
        Session *session = static_cast<Session *>(userdata);

        static const Flag descr[] = {
            { NE_SSL_NOTYETVALID, "certificate not yet valid" },
            { NE_SSL_EXPIRED, "certificate has expired" },
            { NE_SSL_IDMISMATCH, "hostname mismatch" },
            { NE_SSL_UNTRUSTED, "untrusted certificate" },
            { 0, NULL }
        };

        SE_LOG_DEBUG(NULL, NULL,
                     "%s: SSL verification problem: %s",
                     session->getURL().c_str(),
                     Flags2String(failures, descr).c_str());
        if (!session->m_settings->verifySSLCertificate()) {
            SE_LOG_DEBUG(NULL, NULL, "ignoring bad certificate");
            return 0;
        }
        if (failures == NE_SSL_IDMISMATCH &&
            !session->m_settings->verifySSLHost()) {
            SE_LOG_DEBUG(NULL, NULL, "ignoring hostname mismatch");
            return 0;
        }
        return 1;
    } catch (...) {
        Exception::handle();
        return 1;
    }
}

unsigned int Session::options()
{
    unsigned int caps;
    check(ne_options2(m_session, m_uri.m_path.c_str(), &caps));
    return caps;
}

void Session::check(int error)
{
    if (error) {
        SE_THROW_EXCEPTION(TransportException,
                           StringPrintf("Neon error code %d: %s",
                                        error,
                                        ne_get_error(m_session)));
    }
}

}

SE_END_CXX

