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

/**
 * This file contains everything that a D-Bus server needs to
 * integrate a normal C++ class into D-Bus. Argument and result
 * marshaling is done in wrapper functions which convert directly
 * to normal C++ types (bool, integers, std::string, std::map<>, ...).
 * See dbus_traits for the full list of supported types.
 *
 * Before explaining the binding, some terminology first:
 * - A function has a return type and multiple parameters.
 * - Input parameters are read-only arguments of the function.
 * - The function can return values to the caller via the
 *   return type and output parameters (retvals).
 *
 * The C++ binding roughly looks like this:
 * - Arguments can be passed as plain types or const references:
     void foo(int arg); void bar(const std::string &str);
 * - A single result can be returned as return value:
 *   int foo();
 * - Multiple results can be copied into instances provided by
 *   the wrapper, passed by reference: void foo(std::string &res);
 * - A return value, arguments and retvals can be combined
 *   arbitrarily. In the D-Bus reply the return code comes before
 *   all return values.
 *
 * Asynchronous methods are possible by declaring one parameter as a
 * Result pointer and later calling the virtual function provided by
 * it. Parameter passing of results is less flexible than that of
 * method parameters: the later allows both std::string as well as
 * const std::string &, for results only the const reference is
 * supported. The Result instance is passed as pointer and then owned
 * by the called method.
 *
 * Reference counting via boost::intrusive_ptr ensures that all
 * D-Bus objects are handled automatically internally.
 */


#ifndef INCL_GDBUS_CXX_BRIDGE
#define INCL_GDBUS_CXX_BRIDGE
#include "gdbus-cxx.h"

#include <gio/gio.h>

#include <map>
#include <vector>

#include <boost/bind.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>
#include <boost/algorithm/string/predicate.hpp>

/* The connection and message are the only client-exposed types from
 * the C API. To keep changes to a minimum while supporting both dbus
 * implementations, this is made to be a define. The intention is to
 * remove the define once the in-tree gdbus is dropped. */
#define DBUS_CONNECTION_TYPE GDBusConnection
#define DBUS_MESSAGE_TYPE    GDBusMessage
#define DBUS_NEW_ERROR_MSG   g_dbus_message_new_method_error

namespace boost {
    void intrusive_ptr_add_ref(GDBusConnection       *con);
    void intrusive_ptr_release(GDBusConnection       *con);
    void intrusive_ptr_add_ref(GDBusMessage          *msg);
    void intrusive_ptr_release(GDBusMessage          *msg);
}

namespace GDBusCXX {

class DBusConnectionPtr : public boost::intrusive_ptr<GDBusConnection>
{
 public:
    DBusConnectionPtr() {}
    // connections are typically created once, so increment the ref counter by default
    DBusConnectionPtr(GDBusConnection *conn, bool add_ref = true) :
      boost::intrusive_ptr<GDBusConnection>(conn, add_ref)
      {}

    GDBusConnection *reference(void) throw()
    {
        GDBusConnection *conn = get();
        g_object_ref(conn);
        return conn;
    }
};

class GDBusMessagePtr : public boost::intrusive_ptr<GDBusMessage>
{
 public:
    GDBusMessagePtr() {}
    // expected to be used for messages created anew,
    // so use the reference already incremented for us
    // and don't increment by default
    GDBusMessagePtr(GDBusMessage *msg, bool add_ref = false) :
      boost::intrusive_ptr<GDBusMessage>(msg, add_ref)
    {}

    GDBusMessage *reference(void) throw()
    {
        GDBusMessage *msg = get();
        g_object_ref(msg);
        return msg;
    }
};

/**
 * wrapper around GError which initializes
 * the struct automatically, then can be used to
 * throw an exception
 */
class DBusErrorCXX : public GError
{
 public:
    void throwFailure(const std::string &operation, const std::string &explanation = " failed")
    {
        throw std::runtime_error(operation + explanation);
    }
};

GDBusConnection *dbus_get_bus_connection(const char *busType,
                                          const char *interface,
                                          bool unshared,
                                          DBusErrorCXX *err);

/**
 * Special type for object paths. A string in practice.
 */
class DBusObject_t : public std::string
{
 public:
    DBusObject_t() {}
    template <class T> DBusObject_t(T val) : std::string(val) {}
    template <class T> DBusObject_t &operator = (T val) { assign(val); return *this; }
};

/**
 * specializations of this must defined methods for encoding and
 * decoding type C and declare its signature
 */
template<class C> struct dbus_traits {};

struct dbus_traits_base
{
    /**
     * A C++ method or function can handle a call asynchronously by
     * asking to be passed a "boost::shared_ptr<Result*>" parameter.
     * The dbus_traits for those parameters have "asynchronous" set to
     * true, which skips all processing after calling the method.
     */
    static const bool asynchronous = false;
};

/**
 * Append a varying number of parameters as result to the
 * message, using AppendRetvals(msg) << res1 << res2 << ...;
 *
 * Types can be anything that has a dbus_traits, including
 * types which are normally recognized as input parameters in D-Bus
 * method calls.
 */
class AppendRetvals {
    GDBusMessage *m_msg;
    GVariantBuilder m_builder;

 public:
    AppendRetvals(GDBusMessage *msg) {
        m_msg = msg;
        g_variant_builder_init(&m_builder, G_VARIANT_TYPE_TUPLE);
    }

    ~AppendRetvals()
    {
        g_dbus_message_set_body(m_msg, g_variant_builder_end(&m_builder));
    }

    template<class A> AppendRetvals & operator << (const A &a) {
        dbus_traits<A>::append(m_builder, a);
        return *this;
    }
};

/**
 * Append a varying number of method parameters as result to the reply
 * message, using AppendArgs(msg) << Set<A1>(res1) << Set<A2>(res2) << ...;
 */
struct AppendArgs {
    GDBusMessage *m_msg;
    GVariantBuilder m_builder;

    AppendArgs(GDBusMessage *msg) {
        m_msg = msg;
        g_variant_builder_init(&m_builder, G_VARIANT_TYPE_TUPLE);
    }

    ~AppendArgs() {
        g_dbus_message_set_body(m_msg, g_variant_builder_end(&m_builder));
    }

    /** syntactic sugar: redirect << into Set instance */
    template<class A> AppendArgs & operator << (const A &a) {
        return a.set(*this);
    }

    /**
     * Always append argument, including those types which
     * would be recognized by << as parameters and thus get
     * skipped.
     */
    template<class A> AppendArgs & operator + (const A &a) {
        dbus_traits<A>::append(m_builder, a);
        return *this;
    }
};

/** default: skip it, not a result of the method */
template<class A> struct Set
{
    Set(A &a) {}
    AppendArgs &set(AppendArgs &context) const {
        return context;
    }
};

/** same for const reference */
template<class A> struct Set <const A &>
{
    Set(A &a) {}
    AppendArgs &set(AppendArgs &context) const {
        return context;
    }
};

/** specialization for reference: marshal result */
template<class A> struct Set <A &>
{
    A &m_a;
    Set(A &a) : m_a(a) {}
    AppendArgs &set(AppendArgs &context) const {
        dbus_traits<A>::append(context.m_builder, m_a);
        return context;
    }
};

/**
 * Extract values from a message, using ExtractArgs(conn, msg) >> Get<A1>(val1) >> Get<A2>(val2) >> ...;
 *
 * This complements AppendArgs: it skips over those method arguments
 * which are results of the method. Which values are skipped and
 * which are marshalled depends on the specialization of Get and thus
 * ultimately on the prototype of the method.
 */
struct ExtractArgs {
    GDBusConnection *m_conn;
    GDBusMessage *m_msg;
    GVariantIter m_iter;

 public:
    ExtractArgs(GDBusConnection *conn, GDBusMessage *msg) {
        m_conn = conn;
        m_msg = msg;
        g_variant_iter_init(&m_iter, g_dbus_message_get_body(m_msg));
    }

    /** syntactic sugar: redirect >> into Get instance */
    template<class A> ExtractArgs & operator >> (const A &a) {
        return a.get(*this);
    }
};

/** default: extract data from message */
template<class A> struct Get
{
    A &m_a;
    Get(A &a) : m_a(a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        dbus_traits<A>::get(context.m_conn, context.m_msg, context.m_iter, m_a);
        return context;
    }
};

/** same for const reference */
template<class A> struct Get <const A &>
{
    A &m_a;
    Get(A &a) : m_a(a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        dbus_traits<A>::get(context.m_conn, context.m_msg, context.m_iter, m_a);
        return context;
    }
};

/** specialization for reference: skip it, not an input parameter */
template<class A> struct Get <A &>
{
    Get(A &a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        return context;
    }
};

/**
 * interface expected by EmitSignal
 */
class DBusObject
{
 public:
    virtual ~DBusObject() {}

    virtual GDBusConnection *getConnection() const = 0;
    virtual const char *getPath() const = 0;
    virtual const char *getInterface() const = 0;
};

class EmitSignal0
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal0(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () ()
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                      m_object.getInterface(),
                                                      m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }

        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        entry->name = g_strdup(m_signal.c_str());

        return entry;
    }
};

template <typename Arg>
void appendNewArg(GPtrArray *pa)
{
    // Only append argument if there is a signature.
    if(dbus_traits<Arg>::getSignature().empty()) {
        return;
    }

    GDBusArgInfo *argInfo = g_new0(GDBusArgInfo, 1);
    argInfo->signature = g_strdup(dbus_traits<Arg>::getSignature().c_str());
    g_dbus_arg_info_ref(argInfo);
    g_ptr_array_add(pa, argInfo);
}

template <typename Arg>
void appendNewArgForReply(GPtrArray *pa)
{
    // Only append argument if there is a reply signature.
    if(dbus_traits<Arg>::getReply().empty()) {
        return;
    }

    GDBusArgInfo *argInfo = g_new0(GDBusArgInfo, 1);
    argInfo->signature = g_strdup(dbus_traits<Arg>::getReply().c_str());
    g_dbus_arg_info_ref(argInfo);
    g_ptr_array_add(pa, argInfo);
}

template <typename A1>
class EmitSignal1
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal1(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                      m_object.getInterface(),
                                                      m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }

        AppendRetvals(msg.get()) << a1;

        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];;

        return g_dbus_signal_info_ref(entry);
    }
};

template <typename A1, typename A2>
class EmitSignal2
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal2(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1, A2 a2)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                      m_object.getInterface(),
                                                      m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2;

        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];

        return g_dbus_signal_info_ref(entry);
    }
};

template <typename A1, typename A2, typename A3>
class EmitSignal3
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal3(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1, A2 a2, A3 a3)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                     m_object.getInterface(),
                                                     m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2 << a3;
        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];;

        return g_dbus_signal_info_ref(entry);
    }
};

template <typename A1, typename A2, typename A3, typename A4>
class EmitSignal4
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal4(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                     m_object.getInterface(),
                                                     m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2 << a3 << a4;
        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];;

        return g_dbus_signal_info_ref(entry);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5>
class EmitSignal5
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal5(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                     m_object.getInterface(),
                                                     m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2 << a3 << a4 << a5;
        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        appendNewArg<A5>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];;

        return g_dbus_signal_info_ref(entry);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
class EmitSignal6
{
    const DBusObject &m_object;
    const std::string m_signal;

 public:
    EmitSignal6(const DBusObject &object,
                const std::string &signal) :
        m_object(object),
        m_signal(signal)
    {}

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    {
        GDBusMessagePtr msg(g_dbus_message_new_signal(m_object.getPath(),
                                                     m_object.getInterface(),
                                                     m_signal.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_signal() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2 << a3 << a4 << a5 << a6;
        if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message failed");
        }
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_sized_new(7);
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        appendNewArg<A5>(args);
        appendNewArg<A6>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(m_signal.c_str());
        entry->args = (GDBusArgInfo **)args->pdata[args->len - 1];;

        return g_dbus_signal_info_ref(entry);
    }
};

struct MethodHandler
{
    typedef GDBusMessage *(*MethodFunction)(GDBusConnection *conn, GDBusMessage *msg, void *data);
    static std::map<const std::string, std::pair<MethodFunction, void*> > m_methodMap;

    static void handler(GDBusConnection       *connection,
                        const gchar           *sender,
                        const gchar           *object_path,
                        const gchar           *interface_name,
                        const gchar           *method_name,
                        GVariant              *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
    {
        std::map<const std::string, std::pair<MethodFunction, void*> >::iterator it;
        it = m_methodMap.find(method_name);
        if(it == m_methodMap.end()) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                                       "org.SyncEvolution.NoMatchingMethodName",
                                                       "No methods registered with this name");
            return;
        }

        MethodFunction methodFunc = it->second.first;
        void *methodData          = it->second.second;
        GDBusMessage *reply;
        reply = (methodFunc)(g_dbus_method_invocation_get_connection(invocation),
                             g_dbus_method_invocation_get_message(invocation),
                             methodData);
        
        GError *error = NULL;
        g_dbus_connection_send_message(g_dbus_method_invocation_get_connection(invocation),
                                       reply,
                                       G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                       NULL,
                                       &error);
    }
};

template <class M>
struct MakeMethodEntry
{
    // There is no generic implementation of this method.
    // If you get an error about it missing, then write
    // a specialization for your type M (the method pointer).
    //
    // static GDBusMethodEntry make(const char *name)
};

/**
 * utility class for registering an interface
 */
class DBusObjectHelper : public DBusObject
{
    DBusConnectionPtr m_conn;
    guint m_connId;
    std::string m_path;
    std::string m_interface;
    boost::function<void (void)> m_callback;
    bool m_activated;
    GPtrArray *m_methods;
    GPtrArray *m_signals;

 public:
    DBusObjectHelper(GDBusConnection *conn,
                     const std::string &path,
                     const std::string &interface,
                     const boost::function<void (void)> &callback = boost::function<void (void)>()) :
        m_conn(conn),
        m_path(path),
        m_interface(interface),
        m_callback(callback),
        m_activated(false)
    {
    }

    ~DBusObjectHelper()
    {
        deactivate();
    }

    virtual GDBusConnection *getConnection() const { return m_conn.get(); }
    virtual const char *getPath() const { return m_path.c_str(); }
    virtual const char *getInterface() const { return m_interface.c_str(); }

    /**
     * binds a member to the this pointer of its instance
     * and invokes it when the specified method is called
     */
    template <class A1, class C, class M> void add(A1 instance, M C::*method, const char *name)
    {
        // TODO: cleanup method
        typedef MakeMethodEntry< boost::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));
        MethodHandler::m_methodMap.insert(std::pair<const std::string,
                                           std::pair<MethodHandler::MethodFunction,
                                           void*> >(name,
                                                    std::pair<MethodHandler::MethodFunction,
                                                    void*>(entry_type::methodFunction,
                                                           new boost::function<M>(entry_type::boostptr(method,
                                                                                                       instance)))));
    }

    /**
     * binds a plain function pointer with no additional arguments and
     * invokes it when the specified method is called
     */
    template <class M> void add(M *function, const char *name)
    {
        typedef MakeMethodEntry< boost::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));
        MethodHandler::m_methodMap.insert(std::pair<const std::string,
                                           std::pair<MethodHandler::MethodFunction,
                                           void*> >(name,
                                                    std::pair<MethodHandler::MethodFunction,
                                                    void*>(entry_type::methodFunction,
                                                           new boost::function<M>(function))));
    }

    /**
     * add an existing signal entry
     */
    template <class S> void add(const S &s)
    {
        g_ptr_array_add(m_signals, s.makeSignalEntry());
    }

    void activate(GDBusMethodInfo   **methods,
                  GDBusSignalInfo   **signals,
                  GDBusPropertyInfo **properties,
                  const boost::function<void (void)> &callback)
    {
        GDBusInterfaceInfo *ifInfo = g_new0(GDBusInterfaceInfo, 1);
        ifInfo->name       = g_strdup(m_interface.c_str());
        ifInfo->methods    = methods;
        ifInfo->signals    = signals;
        ifInfo->properties = properties;

        GDBusInterfaceVTable *ifVTable = g_new0(GDBusInterfaceVTable, 1);
        ifVTable->method_call = MethodHandler::handler;

        if ((m_connId = g_dbus_connection_register_object(getConnection(),
                                                          getPath(),
                                                          ifInfo,
                                                          ifVTable,
                                                          this,
                                                          NULL,
                                                          NULL)) == 0) {
            throw std::runtime_error(std::string("g_dbus_connection_register_object() failed for ") +
                                     getPath() + " " + getInterface());
        }
        m_callback = callback;
        m_activated = true;
    }

    void activate() {
        GDBusInterfaceInfo *ifInfo = g_new0(GDBusInterfaceInfo, 1);
        ifInfo->name       = g_strdup(m_interface.c_str());
        ifInfo->methods    = (GDBusMethodInfo **)(m_methods);
        ifInfo->signals    = (GDBusSignalInfo **)(m_signals);

        GDBusInterfaceVTable *ifVTable = g_new0(GDBusInterfaceVTable, 1);
        ifVTable->method_call = MethodHandler::handler;

        if ((m_connId = g_dbus_connection_register_object(getConnection(),
                                                          getPath(),
                                                          ifInfo,
                                                          ifVTable,
                                                          this,
                                                          NULL,
                                                          NULL)) == 0) {
            throw std::runtime_error(std::string("g_dbus_connection_register_object() failed for ") +
                                     getPath() + " " + getInterface());
        }
        m_activated = true;
    }

    void deactivate()
    {
        if (m_activated) {
            if (!g_dbus_connection_unregister_object(getConnection(), m_connId)) {
                throw std::runtime_error(std::string("g_dbus_connection_unregister_object() failed for ") +
                                         getPath() + " " + getInterface());
            }
            m_activated = false;
        }
    }
    static void interfaceCallback(void *userData) {
        DBusObjectHelper* helper = static_cast<DBusObjectHelper*>(userData);
        if(!helper->m_callback.empty()) {
            helper->m_callback();
        }
    }
};


/**
 * to be used for plain parameters like int32_t:
 * treat as arguments which have to be extracted
 * from the GVariants and can be skipped when
 * encoding the reply
 */

struct VariantTypeBoolean { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_BOOLEAN; } };
struct VariantTypeByte    { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_BYTE;    } };
struct VariantTypeInt16   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT16;   } };
struct VariantTypeUInt16  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT16;  } };
struct VariantTypeInt32   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT32;   } };
struct VariantTypeUInt32  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT32;  } };
struct VariantTypeInt64   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT64;   } };
struct VariantTypeUInt64  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT64;  } };
struct VariantTypeDouble  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_DOUBLE;  } };

template<class host, class VariantTraits> struct basic_marshal : public dbus_traits_base
{
    typedef host host_type;
    typedef host arg_type;

    /**
     * copy value from GVariant iterator into variable
     */
    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host &value)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_equal(g_variant_get_type(var), VariantTraits::getVariantType())) {
            throw std::runtime_error("invalid argument");
        }
        g_variant_get(var, g_variant_get_type_string(var), &value);
        g_variant_unref(var);
    }

    /**
     * copy value into D-Bus iterator
     */
    static void append(GVariantBuilder &builder, arg_type value)
    {
        const gchar *typeStr = g_variant_type_dup_string(VariantTraits::getVariantType());
        g_variant_builder_add(&builder, typeStr, &value);
        g_free((gpointer)typeStr);
    }
};

template<> struct dbus_traits<uint8_t> :
public basic_marshal< uint8_t, VariantTypeByte >
{
    /**
     * plain type, regardless of whether used as
     * input or output parameter
     */
    static std::string getType() { return "y"; }

    /**
     * plain type => input parameter => non-empty signature
     */
    static std::string getSignature() {return getType(); }

    /**
     * plain type => not returned to caller
     */
    static std::string getReply() { return ""; }

};

/** if the app wants to use signed char, let it and treat it like a byte */
template<> struct dbus_traits<int8_t> : dbus_traits<uint8_t>
{
    typedef int8_t host_type;
    typedef int8_t arg_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &value)
    {
        dbus_traits<uint8_t>::get(conn, msg, iter, reinterpret_cast<uint8_t &>(value));
    }
};

template<> struct dbus_traits<int16_t> :
    public basic_marshal< int16_t, VariantTypeInt16 >
{
    static std::string getType() { return "n"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<uint16_t> :
    public basic_marshal< uint16_t, VariantTypeUInt16 >
{
    static std::string getType() { return "q"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<int32_t> :
    public basic_marshal< int32_t, VariantTypeInt32 >
{
    static std::string getType() { return "i"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<uint32_t> :
    public basic_marshal< uint32_t, VariantTypeUInt32 >
{
    static std::string getType() { return "u"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};

template<> struct dbus_traits<bool> :
    public basic_marshal< bool, VariantTypeBoolean >
{
    static std::string getType() { return "b"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};

template<> struct dbus_traits<std::string> : public dbus_traits_base
{
    static std::string getType() { return "s"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, std::string &value)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_STRING)) {
            throw std::runtime_error("invalid argument");
        }
        const char *str = g_variant_get_string(var, NULL);
        value = str;

        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, const std::string &value)
    {
        g_variant_builder_add_value(&builder, g_variant_new_string(value.c_str()));
    }

    typedef std::string host_type;
    typedef const std::string &arg_type;
};

template <> struct dbus_traits<DBusObject_t> : public dbus_traits_base
{
    static std::string getType() { return "o"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, DBusObject_t &value)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_OBJECT_PATH)) {
            throw std::runtime_error("invalid argument");
        }
        const char *objPath = g_variant_get_string(var, NULL);
        value = objPath;
        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, const DBusObject_t &value)
    {
        if(!g_variant_is_object_path(value.c_str())) {
            throw std::runtime_error("invalid argument");
        }
        g_variant_builder_add_value(&builder, g_variant_new_string(value.c_str()));
    }

    typedef DBusObject_t host_type;
    typedef const DBusObject_t &arg_type;
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits<Caller_t> : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, Caller_t &value)
    {
        const char *peer = g_dbus_message_get_sender(msg);
        if (!peer) {
            throw std::runtime_error("D-Bus method call without sender?!");
        }
        value = peer;
    }

    typedef Caller_t host_type;
    typedef const Caller_t &arg_type;
};

/**
 * Pass array of basic type plus its number of entries.
 * Can only be used in cases where the caller owns the
 * memory and can discard it when the call returns, in
 * other words, for method calls, asynchronous replys and
 * signals, but not for return values.
 */
template<class V> struct dbus_traits< std::pair<size_t, const V *> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<V>::getType();
    }
    static std::string getType()
    {
        return std::string("a") +
            dbus_traits<V>::getType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::pair<size_t, const V *> host_type;
    typedef const host_type &arg_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &array)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("invalid argument");
        }
        gsize nelements;
        typename dbus_traits<V>::host_type *data;
        data = (typename dbus_traits<V>::host_type *) g_variant_get_fixed_array(var,
                                                                                &nelements,
                                                                                static_cast<gsize>(sizeof(host_type)));
        array.first = nelements;
        array.second = data;
        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, arg_type array)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        for(uint i = 0; i < array.first; ++i) {
            g_variant_builder_add(&builder, getContainedType().c_str(), &array.second[i]);
        }
        g_variant_builder_close(&builder);
    }
};

/**
 * a std::map - treat it like a D-Bus dict
 */
template<class K, class V> struct dbus_traits< std::map<K, V> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return std::string("{") +
            dbus_traits<K>::getType() +
            dbus_traits<V>::getType() +
            "}";
    }
    static std::string getType()
    {
        return std::string("a") +
            getContainedType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::map<K, V> host_type;
    typedef const std::map<K, V> &arg_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &dict)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("invalid argument");
        }

        GVariantIter contIter;
        GVariant *child;
        g_variant_iter_init(&contIter, var);
        while((child = g_variant_iter_next_value(&contIter)) != NULL) {
            K key;
            V value;
            GVariantIter childIter;
            g_variant_iter_init(&childIter, child);
            dbus_traits<K>::get(conn, msg, childIter, key);
            dbus_traits<V>::get(conn, msg, childIter, value);
            dict.insert(std::make_pair(key, value));
            g_variant_unref(child);
        }
        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, arg_type dict)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));

        for(typename host_type::const_iterator it = dict.begin();
            it != dict.end();
            ++it) {
            g_variant_builder_open(&builder, G_VARIANT_TYPE(getContainedType().c_str()));
            dbus_traits<K>::append(builder, it->first);
            dbus_traits<V>::append(builder, it->second);
            g_variant_builder_close(&builder);
        }

        g_variant_builder_close(&builder);
    }
};

/**
 * a std::vector - maps to D-Bus array, but with inefficient marshaling
 * because we cannot get a base pointer for the whole array
 */
template<class V> struct dbus_traits< std::vector<V> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<V>::getType();
    }
    static std::string getType()
    {
        return std::string("a") +
            getContainedType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::vector<V> host_type;
    typedef const std::vector<V> &arg_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &array)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("invalid argument");
        }

        int nelements = g_variant_n_children(var);
        for(int i = 0; i < nelements; ++i) {
            GVariant *childVar = g_variant_get_child_value(var, i);
            GVariantIter childIter;
            V value;
            g_variant_iter_init(&childIter, childVar);
            dbus_traits<V>::get(conn, msg, childIter, value);
            array.push_back(value);
        }
        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, arg_type array)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getContainedType().c_str()));

        for(typename host_type::const_iterator it = array.begin();
            it != array.end();
            ++it) {
            dbus_traits<V>::append(builder, *it);
        }

        g_variant_builder_close(&builder);
    }
};

/**
 * A boost::variant <V> maps to a dbus variant, only care about values of
 * type V but will not throw error if type is not matched, this is useful if
 * application is interested on only a sub set of possible value types
 * in variant.
 */
template <class V> struct dbus_traits <boost::variant <V> > : public dbus_traits_base
{
    static std::string getType() { return "v"; }
    static std::string getSignature() { return getType(); }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, boost::variant <V> &value)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("invalid argument");
        }

        GVariant *varVar;
        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        varVar = g_variant_iter_next_value(&varIter);
        if (!boost::iequals(g_variant_get_type_string(varVar), dbus_traits<V>::getSignature())) {
            //ignore unrecognized sub type in variant
            return;
        }

        V val;
        // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
        g_variant_iter_init(&varIter, var);
        dbus_traits<V>::get(conn, msg, varIter, val);
        value = val;

        g_variant_unref(var);
        g_variant_unref(varVar);
    }

    typedef boost::variant<V> host_type;
    typedef const boost::variant<V> &arg_type;
};

/**
 * A boost::variant <V1, V2> maps to a dbus variant, only care about values of
 * type V1, V2 but will not throw error if type is not matched, this is useful if
 * application is interested on only a sub set of possible value types
 * in variant.
 */
template <class V1, class V2> struct dbus_traits <boost::variant <V1, V2> > : public dbus_traits_base
{
    static std::string getType() { return "v"; }
    static std::string getSignature() { return getType(); }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, boost::variant <V1, V2> &value)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("invalid argument");
        }

        host_type bVar;
        GVariant *varVar;
        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        varVar = g_variant_iter_next_value(&varIter);
        if (!boost::iequals(g_variant_get_type_string(varVar), dbus_traits<V2>::getSignature())) {
            // ignore unrecognized sub type in variant
            return;
        }
        else if (boost::iequals(g_variant_get_type_string(varVar), dbus_traits<V1>::getSignature())) {
            V1 val;
            // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V1>::get(conn, msg, varIter, val);
            value = val;
        } else {
            V2 val;
            dbus_traits<V2>::get(conn, msg, varIter, val);
            value = val;
        }
        g_variant_unref(var);
        g_variant_unref(varVar);
    }

    typedef boost::variant<V1, V2> host_type;
    typedef const boost::variant<V1, V2> &arg_type;
};

/**
 * a single member m of type V in a struct K
 */
template<class K, class V, V K::*m> struct dbus_member_single
{
    static std::string getType()
    {
        return dbus_traits<V>::getType();
    }
    typedef V host_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, K &val)
    {
        dbus_traits<V>::get(conn, msg, iter, val.*m);
    }

    static void append(GVariantBuilder &builder, const K &val)
    {
        dbus_traits<V>::append(builder, val.*m);
    }
};

/**
 * a member m of type V in a struct K, followed by another dbus_member
 * or dbus_member_single to end the chain
 */
template<class K, class V, V K::*m, class M> struct dbus_member
{
    static std::string getType()
    {
        return dbus_traits<V>::getType() + M::getType();
    }
    typedef V host_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, K &val)
    {
        dbus_traits<V>::get(conn, msg, iter, val.*m);
        M::get(conn, msg, iter, val);
    }

    static void append(GVariantBuilder &builder, const K &val)
    {
        dbus_traits<V>::append(builder, val.*m);
        M::append(builder, val);
    }
};

/**
 * a helper class which implements dbus_traits for
 * a class, use with:
 * struct foo { int a; std::string b;  };
 * template<> struct dbus_traits< foo > : dbus_struct_traits< foo,
 *                                                            dbus_member<foo, int, &foo::a,
 *                                                            dbus_member_single<foo, std::string, &foo::b> > > {};
 */
template<class K, class M> struct dbus_struct_traits : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return M::getType();
    }
    static std::string getType()
    {
        return std::string("(") +
            getContainedType() +
            ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef K host_type;
    typedef const K &arg_type;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &val)
    {
        GVariant *var = g_variant_iter_next_value(&iter);
        if (var == NULL || g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("invalid argument");
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        M::get(conn, msg, tupIter, val);

        g_variant_unref(var);
    }

    static void append(GVariantBuilder &builder, arg_type val)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getContainedType().c_str()));
        M::append(builder, val);
        g_variant_builder_close(&builder);
    }
};

/**
 * special case const reference parameter:
 * treat like pass-by-value input argument
 *
 * Example: const std::string &arg
 */
template<class C> struct dbus_traits<const C &> : public dbus_traits<C> {};

/**
 * special case writeable reference parameter:
 * must be a return value
 *
 * Example: std::string &retval
 */
template<class C> struct dbus_traits<C &> : public dbus_traits<C>
{
    static std::string getSignature() { return ""; }
    static std::string getReply() { return dbus_traits<C>::getType(); }
};

/**
 * dbus-cxx base exception thrown in dbus server
 * org.syncevolution.gdbus-cxx.Exception
 * This base class only contains interfaces, no data members
 */
class DBusCXXException
{
 public:
    /**
     * get exception name, used to convert to dbus error name
     * subclasses should override it
     */
    virtual std::string getName() const { return "org.syncevolution.gdbus-cxx.Exception"; }

    /**
     * get error message
     */
    virtual const char* getMessage() const { return "unknown"; }
};

static GDBusMessage *handleException(GDBusMessage *msg)
{
    try {
#ifdef DBUS_CXX_EXCEPTION_HANDLER
        return DBUS_CXX_EXCEPTION_HANDLER(msg);
#else
        throw;
#endif
    } catch (const dbus_error &ex) {
        return g_dbus_message_new_method_error(msg, ex.dbusName().c_str(), "%s", ex.what());
    } catch (const DBusCXXException &ex) {
        return g_dbus_message_new_method_error(msg, ex.getName().c_str(), "%s", ex.getMessage());
    } catch (const std::runtime_error &ex) {
        return g_dbus_message_new_method_error(msg, "org.syncevolution.gdbus-cxx.Exception", "%s", ex.what());
    } catch (...) {
        return g_dbus_message_new_method_error(msg, "org.syncevolution.gdbus-cxx.Exception", "unknown");
    }
}

/**
 * Check presence of a certain D-Bus client.
 */
class DBusWatch : public Watch
{
    DBusConnectionPtr m_conn;
    boost::function<void (void)> m_callback;
    bool m_called;
    guint m_watchID;

    static void disconnect(GDBusConnection *connection,
                           const gchar *sender_name,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *signal_name,
                           GVariant *parameters,
                           gpointer user_data)
    {
        DBusWatch *watch = static_cast<DBusWatch *>(user_data);
        if (!watch->m_called) {
            watch->m_called = true;
            if (watch->m_callback) {
                watch->m_callback();
            }
        }
    }

 public:
    DBusWatch(const DBusConnectionPtr &conn,
              const boost::function<void (void)> &callback = boost::function<void (void)>()) :
        m_conn(conn),
        m_callback(callback),
        m_called(false),
        m_watchID(0)
    {
    }

    virtual void setCallback(const boost::function<void (void)> &callback)
    {
        m_callback = callback;
        if (m_called && m_callback) {
            m_callback();
        }
    }

    void activate(const char *peer)
    {
        if (!peer) {
            throw std::runtime_error("DBusWatch::activate(): no peer");
        }

        // Install watch first ...
        m_watchID = g_dbus_connection_signal_subscribe(m_conn.get(),
                                                       peer,
                                                       "org.freedesktop.DBus",
                                                       "NameLost",
                                                       "/org/freesktop/DBus",
                                                       NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       disconnect,
                                                       this,
                                                       NULL);
        if (!m_watchID) {
            throw std::runtime_error("g_dbus_connection_signal_subscribe(): NameLost failed");
        }

        // ... then check that the peer really exists,
        // otherwise we'll never notice the disconnect.
        // If it disconnects while we are doing this,
        // then disconnect() will be called twice,
        // but it handles that.
        GError *error = NULL;

        GVariant *result = g_dbus_connection_call_sync(m_conn.get(),
                                                       "org.freedesktop.DBus",
                                                       "/org/freedesktop/DBus",
                                                       "org.freedesktop.DBus",
                                                       "NameHasOwner",
                                                       g_variant_new ("(s)", peer),
                                                       G_VARIANT_TYPE ("(b)"),
                                                       G_DBUS_CALL_FLAGS_NONE,
                                                       -1,
                                                       NULL,
                                                       &error);
        if (result == NULL || !g_variant_get_boolean(result)) {
            if(error != NULL) {
                throw std::runtime_error("g_dbus_connection_call_sync(): NameHasOwner");
            }
            disconnect(m_conn.get(), NULL, NULL, NULL, NULL, NULL, this);
        }
    }

    ~DBusWatch()
    {
        if (m_watchID) {
            g_dbus_connection_signal_unsubscribe(m_conn.get(), m_watchID);
            m_watchID = 0;
        }
    }
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits< boost::shared_ptr<Watch> >  : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, boost::shared_ptr<Watch> &value)
    {
        boost::shared_ptr<DBusWatch> watch(new DBusWatch(conn));
        watch->activate(g_dbus_message_get_sender(msg));
        value = watch;
    }

    static void append(GVariantBuilder &builder, const boost::shared_ptr<Watch> &value) {}

    typedef boost::shared_ptr<Watch> host_type;
    typedef const boost::shared_ptr<Watch> &arg_type;
};

/**
 * base class for D-Bus results,
 * keeps references to required objects and provides the
 * failed() method
 */
class DBusResult : virtual public Result
{
 protected:
    DBusConnectionPtr m_conn;     /**< connection via which the message was received */
    GDBusMessagePtr m_msg;         /**< the method invocation message */

 public:
    DBusResult(GDBusConnection *conn,
               GDBusMessage *msg) :
        m_conn(conn, true),
        m_msg(msg, true)
    {}

    virtual void failed(const dbus_error &error)
    {
        GDBusMessage *errMsg;
        errMsg = g_dbus_message_new_method_error(m_msg.get(), error.dbusName().c_str(),
                                                 "%s", error.what());
        if (!g_dbus_connection_send_message(m_conn.get(), m_msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error(" g_dbus_connection_send_message failed");
        }
    }

    virtual Watch *createWatch(const boost::function<void (void)> &callback)
    {
        std::auto_ptr<DBusWatch> watch(new DBusWatch(m_conn, callback));
        watch->activate(g_dbus_message_get_sender(m_msg.get()));
        return watch.release();
    }
};

class DBusResult0 :
    public Result0,
    public DBusResult
{
 public:
    DBusResult0(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done()
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
            throw std::runtime_error("g_dbus_connection_send_message");
        }
    }

    static std::string getSignature() { return ""; }
};

template <typename A1>
class DBusResult1 :
    public Result1<A1>,
    public DBusResult
{
 public:
    DBusResult1(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() { return dbus_traits<A1>::getSignature(); }

    static const bool asynchronous = dbus_traits<A1>::asynchronous;
};

template <typename A1, typename A2>
class DBusResult2 :
    public Result2<A1, A2>,
    public DBusResult
{
 public:
    DBusResult2(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult1<A2>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult1<A2>::asynchronous;
};

template <typename A1, typename A2, typename A3>
class DBusResult3 :
    public Result3<A1, A2, A3>,
    public DBusResult
{
 public:
    DBusResult3(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult2<A2, A3>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult2<A2, A3>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4>
class DBusResult4 :
    public Result4<A1, A2, A3, A4>,
    public DBusResult
{
 public:
    DBusResult4(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }
    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult3<A2, A3, A4>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult3<A2, A3, A4>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5>
class DBusResult5 :
    public Result5<A1, A2, A3, A4, A5>,
    public DBusResult
{
 public:
    DBusResult5(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult4<A2, A3, A4, A5>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult4<A2, A3, A4, A5>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6>
class DBusResult6 :
    public Result6<A1, A2, A3, A4, A5, A6>,
    public DBusResult
{
 public:
    DBusResult6(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5 << a6;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult5<A2, A3, A4, A5, A6>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult5<A2, A3, A4, A5, A6>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7>
class DBusResult7 :
    public Result7<A1, A2, A3, A4, A5, A6, A7>,
    public DBusResult
{
 public:
    DBusResult7(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5 << a6 << a7;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult6<A2, A3, A4, A5, A6, A7>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult6<A2, A3, A4, A5, A6, A7>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8>
class DBusResult8 :
    public Result8<A1, A2, A3, A4, A5, A6, A7, A8>,
    public DBusResult
{
 public:
    DBusResult8(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult7<A2, A3, A4, A5, A6, A7, A8>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult7<A2, A3, A4, A5, A6, A7, A8>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8, typename A9>
class DBusResult9 :
    public Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9>,
    public DBusResult
{
 public:
    DBusResult9(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult8<A2, A3, A4, A5, A6, A7, A8, A9>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult8<A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8, typename A9, typename A10>
class DBusResult10 :
    public Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10>,
    public DBusResult
{
 public:
    DBusResult10(GDBusConnection *conn,
                 GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10)
    {
        GDBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply.get()) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
        if (!g_dbus_connection_send_message(m_conn.get(), reply.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL)) {
        }
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult9<A2, A3, A4, A5, A6, A7, A8, A9, A10>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult9<A2, A3, A4, A5, A6, A7, A8, A9, A10>::asynchronous;
};

/**
 * A parameter which points towards one of our Result* structures.
 * All of the types contained in it count towards the Reply signature.
 * The requested Result type itself is constructed here.
 *
 * @param R      Result0, Result1<type>, ...
 * @param DBusR  the class implementing R
 */
template <class R, class DBusR> struct dbus_traits_result
{
    static std::string getType() { return DBusR::getSignature(); }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return getType(); }

    typedef boost::shared_ptr<R> host_type;
    typedef boost::shared_ptr<R> &arg_type;
    static const bool asynchronous = true;

    static void get(GDBusConnection *conn, GDBusMessage *msg,
                    GVariantIter &iter, host_type &value)
    {
        value.reset(new DBusR(conn, msg));
    }
};

template <>
struct dbus_traits< boost::shared_ptr<Result0> > :
    public dbus_traits_result<Result0, DBusResult0>
{};
template <class A1>
struct dbus_traits< boost::shared_ptr< Result1<A1> > >:
    public dbus_traits_result< Result1<A1>, DBusResult1<A1> >
{};
template <class A1, class A2>
struct dbus_traits< boost::shared_ptr< Result2<A1, A2> > >:
    public dbus_traits_result< Result2<A1, A2>, DBusResult2<A1, A2> >
{};
template <class A1, class A2, class A3>
    struct dbus_traits< boost::shared_ptr< Result3<A1, A2, A3> > >:
    public dbus_traits_result< Result3<A1, A2, A3>, DBusResult3<A1, A2, A3> >
{};
template <class A1, class A2, class A3, class A4>
    struct dbus_traits< boost::shared_ptr< Result4<A1, A2, A3, A4> > >:
    public dbus_traits_result< Result4<A1, A2, A3, A4>, DBusResult4<A1, A2, A3, A4> >
{};
template <class A1, class A2, class A3, class A4, class A5>
    struct dbus_traits< boost::shared_ptr< Result5<A1, A2, A3, A4, A5> > >:
    public dbus_traits_result< Result5<A1, A2, A3, A4, A5>, DBusResult5<A1, A2, A3, A4, A5> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6>
    struct dbus_traits< boost::shared_ptr< Result6<A1, A2, A3, A4, A5, A6> > >:
    public dbus_traits_result< Result6<A1, A2, A3, A4, A5, A6>, DBusResult6<A1, A2, A3, A4, A5, A6> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    struct dbus_traits< boost::shared_ptr< Result7<A1, A2, A3, A4, A5, A6, A7> > >:
    public dbus_traits_result< Result7<A1, A2, A3, A4, A5, A6, A7>, DBusResult7<A1, A2, A3, A4, A5, A6, A7> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    struct dbus_traits< boost::shared_ptr< Result8<A1, A2, A3, A4, A5, A6, A7, A8> > >:
    public dbus_traits_result< Result8<A1, A2, A3, A4, A5, A6, A7, A8>, DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    struct dbus_traits< boost::shared_ptr< Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9> > >:
    public dbus_traits_result< Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9>, DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10>
    struct dbus_traits< boost::shared_ptr< Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> > >:
    public dbus_traits_result< Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10>, DBusResult10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> >
{};

/** ===> 10 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9, class A10>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8, A9, A10)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M bind(Mptr C::*method, I instance) {
        // this fails because bind() only supports up to 9 parameters, including
        // the initial this pointer
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9 /* _10 */);
    }

    static const bool asynchronous = dbus_traits< DBusResult10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> >::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;
            typename dbus_traits<A10>::host_type a10;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                   >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9) >> Get<A10>(a10);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                    << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9) << Set<A10>(a10);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        appendNewArg<A10>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        appendNewArgForReply<A10>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 9 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7, A8, A9)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        // this fails because bind() only supports up to 9 parameters, including
        // the initial this pointer
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9);
    }

    static const bool asynchronous = DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                   >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                        << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 9 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8, A9)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9);
    }

    static const bool asynchronous = DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                   >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                    << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 8 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7, A8)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8);
    }

    static const bool asynchronous = DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >>
                                      Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                        << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 8 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8);
    }

    static const bool asynchronous = DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                   >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                    << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 7 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7);
    }

    static const bool asynchronous = DBusResult7<A1, A2, A3, A4, A5, A6, A7>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                   >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                        << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 7 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7);
    }

    static const bool asynchronous = DBusResult7<A1, A2, A3, A4, A5, A6, A7>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                   >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                    << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 6 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6);
    }

    static const bool asynchronous = DBusResult6<A1, A2, A3, A4, A5, A6>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                   >> Get<A4>(a4) >> Get<A5>(a5) >> Get<A6>(a6);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                        << Set<A4>(a4) << Set<A5>(a5) << Set<A6>(a6);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 6 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6);
    }

    static const bool asynchronous = DBusResult6<A1, A2, A3, A4, A5, A6>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                   >> Get<A4>(a4) >> Get<A5>(a5) >> Get<A6>(a6);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                    << Set<A4>(a4) << Set<A5>(a5) << Set<A6>(a6);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 5 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5);
    }

    static const bool asynchronous = DBusResult5<A1, A2, A3, A4, A5>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                   >> Get<A4>(a4) >> Get<A5>(a5);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                        << Set<A4>(a4) << Set<A5>(a5);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 5 parameters */
template <class A1, class A2, class A3, class A4, class A5>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5);
    }

    static const bool asynchronous = DBusResult5<A1, A2, A3, A4, A5>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                   >> Get<A4>(a4) >> Get<A5>(a5);

            (*static_cast<M *>(data))(a1, a2, a3, a4, a5);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                    << Set<A4>(a4) << Set<A5>(a5);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 4 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4)> >
{
    typedef R (Mptr)(A1, A2, A3, A4);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4);
    }

    static const bool asynchronous = DBusResult4<A1, A2, A3, A4>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4);

            r = (*static_cast<M *>(data))(a1, a2, a3, a4);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 4 parameters */
template <class A1, class A2, class A3, class A4>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4)> >
{
    typedef void (Mptr)(A1, A2, A3, A4);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4);
    }

    static const bool asynchronous = DBusResult4<A1, A2, A3, A4>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4);

            (*static_cast<M *>(data))(a1, a2, a3, a4);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 3 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3>
struct MakeMethodEntry< boost::function<R (A1, A2, A3)> >
{
    typedef R (Mptr)(A1, A2, A3);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3);
    }

    static const bool asynchronous = DBusResult3<A1, A2, A3>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3);

            r = (*static_cast<M *>(data))(a1, a2, a3);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 3 parameters */
template <class A1, class A2, class A3>
struct MakeMethodEntry< boost::function<void (A1, A2, A3)> >
{
    typedef void (Mptr)(A1, A2, A3);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3);
    }

    static const bool asynchronous = DBusResult3<A1, A2, A3>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3);

            (*static_cast<M *>(data))(a1, a2, a3);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        g_ptr_array_add(inArgs, NULL);
        
        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 2 arguments, 1 return value */
template <class R,
          class A1, class A2>
struct MakeMethodEntry< boost::function<R (A1, A2)> >
{
    typedef R (Mptr)(A1, A2);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2);
    }

    static const bool asynchronous = DBusResult2<A1, A2>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2);

            r = (*static_cast<M *>(data))(a1, a2);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 2 parameters */
template <class A1, class A2>
struct MakeMethodEntry< boost::function<void (A1, A2)> >
{
    typedef void (Mptr)(A1, A2);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2);
    }

    static const bool asynchronous = DBusResult2<A1, A2>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;

            ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2);

            (*static_cast<M *>(data))(a1, a2);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        appendNewArgForReply<A2>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 1 argument, 1 return value */
template <class R,
          class A1>
struct MakeMethodEntry< boost::function<R (A1)> >
{
    typedef R (Mptr)(A1);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1);
    }

    static const bool asynchronous = DBusResult1<A1>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;

            ExtractArgs(conn, msg) >> Get<A1>(a1);

            r = (*static_cast<M *>(data))(a1);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r << Set<A1>(a1);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 1 parameter */
template <class A1>
struct MakeMethodEntry< boost::function<void (A1)> >
{
    typedef void (Mptr)(A1);
    typedef boost::function<void (A1)> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1);
    }

    static const bool asynchronous = DBusResult1<A1>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<A1>::host_type a1;

            ExtractArgs(conn, msg) >> Get<A1>(a1);

            (*static_cast<M *>(data))(a1);

            if (asynchronous) {
                return NULL;
            }

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) << Set<A1>(a1);

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(inArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = strdup(name);
        entry->in_args  = (GDBusArgInfo **)inArgs->pdata[inArgs->len - 1];
        entry->out_args = (GDBusArgInfo **)outArgs->pdata[outArgs->len - 1];

        return g_dbus_method_info_ref(entry);
    }
};

/** 0 arguments, 1 return value */
template <class R>
struct MakeMethodEntry< boost::function<R ()> >
{
    typedef R (Mptr)();
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance);
    }

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                       GDBusMessage *msg, void *data)
    {
        try {
            typename dbus_traits<R>::host_type r;

            r = (*static_cast<M *>(data))();

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;

            AppendArgs(reply) + r;

            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        entry->name     = g_strdup(name);
        entry->in_args  = NULL;
        entry->out_args = NULL;

        return g_dbus_method_info_ref(entry);
    }
};

/** ===> 0 parameter */
template <>
struct MakeMethodEntry< boost::function<void ()> >
{
    typedef void (Mptr)();
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance);
    }

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            (*static_cast<M *>(data))();

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            if (!reply)
                return NULL;
            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    /* static void destroyFunction(void *user_data) */
    /* { */
    /*     BDBusMethodTable *entry = static_cast<BDBusMethodTable *>(user_data); */
    /*     delete static_cast<M *>(entry->method_data); */
    /* } */

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        entry->name     = g_strdup(name);
        entry->in_args  = NULL;
        entry->out_args = NULL;

        return g_dbus_method_info_ref(entry);
    }
};

/**
 * interface to refer to a remote object
 */
class DBusRemoteObject : public DBusObject
{
public:
    virtual const char *getDestination() const = 0;
    virtual ~DBusRemoteObject() {}
};
/**
 * interface expected by DBusClient
 */
class DBusCallObject : public DBusRemoteObject
{
public:
    /* The method name for the calling dbus method */
    virtual const char *getMethod() const =0;
    virtual ~DBusCallObject() {}
};

template <class T>
class DBusClientCall
{
protected:
    const std::string m_destination;
    const std::string m_path;
    const std::string m_interface;
    const std::string m_method;
    const DBusConnectionPtr m_conn;

    typedef GAsyncReadyCallback DBusCallback;
    DBusCallback m_dbusCallback;

     /**
     * called by GDBus to free the user_data pointer set in
     * dbus_pending_call_set_notify()
     */
    static void callDataUnref(void *user_data) {
        delete static_cast<CallbackData *>(user_data);
    }

    typedef T Callback_t;

public:
    struct CallbackData
    {
        //only keep connection, for DBusClientCall instance is absent when 'dbus client call' returns
        //suppose connection is available in the callback handler
        const DBusConnectionPtr m_conn;
        Callback_t m_callback;
        CallbackData(const DBusConnectionPtr &conn, const Callback_t &callback)
            :m_conn(conn), m_callback(callback)
        {}
    };

    DBusClientCall(const DBusCallObject &object, DBusCallback dbusCallback)
        :m_destination (object.getDestination()),
         m_path (object.getPath()),
         m_interface (object.getInterface()),
         m_method (object.getMethod()),
         m_conn (object.getConnection()),
         m_dbusCallback(dbusCallback)
    {
    }

    DBusClientCall(const DBusRemoteObject &object, const std::string &method, DBusCallback dbusCallback)
        :m_destination (object.getDestination()),
         m_path (object.getPath()),
         m_interface (object.getInterface()),
         m_method (method),
         m_conn (object.getConnection()),
         m_dbusCallback(dbusCallback)
    {
    }

    GDBusConnection *getConnection() { return m_conn.get(); }

    void operator () (const Callback_t &callback)
    {
        GDBusMessagePtr msg(g_dbus_message_new_method_call(m_destination.c_str(),
                                                           m_path.c_str(),
                                                           m_interface.c_str(),
                                                           m_method.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_method_call() failed");
        }

        CallbackData *data = new CallbackData(m_conn, callback);

        //parameter marshaling (none)
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  -1, NULL, NULL, m_dbusCallback, data);
    }

    template <class A1>
    void operator () (const A1 &a1, const Callback_t &callback)
    {
        GDBusMessagePtr msg(g_dbus_message_new_method_call(m_destination.c_str(),
                                                           m_path.c_str(),
                                                           m_interface.c_str(),
                                                           m_method.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_method_call() failed");
        }
        AppendRetvals(msg.get()) << a1;

        CallbackData *data = new CallbackData(m_conn, callback);

        //parameter marshaling (none)
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  -1, NULL, NULL, m_dbusCallback, data);
    }

    template <class A1, class A2>
    void operator () (const A1 &a1, const A2 &a2, const Callback_t &callback)
    {
        GDBusMessagePtr msg(g_dbus_message_new_method_call(m_destination.c_str(),
                                                         m_path.c_str(),
                                                         m_interface.c_str(),
                                                         m_method.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_method_call() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2;

        CallbackData *data = new CallbackData(m_conn, callback);

        //parameter marshaling (none)
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  -1, NULL, NULL, m_dbusCallback, data);
    }

    template <class A1, class A2, class A3>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const Callback_t &callback)
    {
        GDBusMessagePtr msg(g_dbus_message_new_method_call(m_destination.c_str(),
                                                           m_path.c_str(),
                                                           m_interface.c_str(),
                                                           m_method.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_method_call() failed");
        }
        AppendRetvals(msg.get()) << a1 << a2 << a3;

        CallbackData *data = new CallbackData(m_conn, callback);

        //parameter marshaling (none)
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  -1, NULL, NULL, m_dbusCallback, data);
    }
};

/*
 * A DBus Client Call object handling zero or more parameter and
 * zero return value.
 */
class DBusClientCall0 : public DBusClientCall<boost::function<void (const std::string &)> >
{
    /**
     * called when result of call is available or an error occurred (non-empty string)
     */
    typedef boost::function<void (const std::string &)> Callback_t;

    /** called by gdbus on error or completion of call */
    static void dbusCallback (GObject *src_obj, GAsyncResult *res, void *user_data)
    {
        typedef typename DBusClientCall<Callback_t>::CallbackData CallbackData;
        CallbackData *data = static_cast<CallbackData *>(user_data);

        GError *err = NULL;
        GDBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &err));
        //unmarshal the return results and call user callback
        (data->m_callback)(err->message);
    }

public:
    DBusClientCall0 (const DBusCallObject &object)
        : DBusClientCall<Callback_t>(object, &DBusClientCall0::dbusCallback)
    {
    }

    DBusClientCall0 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Callback_t>(object, method, &DBusClientCall0::dbusCallback)
    {
    }
};

/** 1 return value and 0 or more parameters */
template <class R1>
class DBusClientCall1 : public DBusClientCall<boost::function<void (const R1 &, const std::string &)> >
{
    /**
     * called when the call is returned or an error occurred (non-empty string)
     */
    typedef boost::function<void (const R1 &, const std::string &)> Callback_t;

    /** called by gdbus on error or completion of call */
    static void dbusCallback (GObject *src_obj, GAsyncResult *res, void *user_data)
    {
        typedef typename DBusClientCall<Callback_t>::CallbackData CallbackData;
        CallbackData *data = static_cast<CallbackData *>(user_data);

        GError *err = NULL;
        GDBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &err));
        typename dbus_traits<R1>::host_type r;
        if (err != NULL) {
            ExtractArgs(data->m_conn.get(), reply.get()) >> Get<R1>(r);
        }

        //unmarshal the return results and call user callback
        //(*static_cast <Callback_t *>(user_data))(r, err->message);
        (data->m_callback)(r, err->message);
    }

public:
    DBusClientCall1 (const DBusCallObject &object)
        : DBusClientCall<Callback_t>(object, &DBusClientCall1::dbusCallback)
    {
    }

    DBusClientCall1 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Callback_t>(object, method, &DBusClientCall1::dbusCallback)
    {
    }
};

/** 2 return value and 0 or more parameters */
template <class R1, class R2>
class DBusClientCall2 : public DBusClientCall<boost::function<
                               void (const R1 &, const R2 &, const std::string &)> >

{
    /**
     * called when the call is returned or an error occurred (non-empty string)
     */
    typedef boost::function<void (const R1 &, const R2 &, const std::string &)> Callback_t;

    /** called by gdbus on error or completion of call */
    static void dbusCallback (GObject *src_obj, GAsyncResult *res, void *user_data)
    {
        typedef typename DBusClientCall<Callback_t>::CallbackData CallbackData;
        CallbackData *data = static_cast<CallbackData *>(user_data);

        GError *err = NULL;
        GDBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &err));
        typename dbus_traits<R1>::host_type r1;
        typename dbus_traits<R2>::host_type r2;
        if (err != NULL) {
            ExtractArgs(data->m_conn.get(), reply.get()) >> Get<R1>(r1) >> Get<R2>(r2);
        }
        //unmarshal the return results and call user callback
        (data->m_callback)(r1, r2, err->message);
    }

public:
    DBusClientCall2 (const DBusCallObject &object)
        : DBusClientCall<Callback_t>(object, &DBusClientCall2::dbusCallback)
    {
    }

    DBusClientCall2 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Callback_t>(object, method, &DBusClientCall2::dbusCallback)
    {
    }
};

/** 3 return value and 0 or more parameters */
template <class R1, class R2, class R3>
class DBusClientCall3 : public DBusClientCall<boost::function<
                               void (const R1 &, const R2 &, const R3 &, const std::string &)> >

{
    /**
     * called when the call is returned or an error occurred (non-empty string)
     */
    typedef boost::function<void (const R1 &, const R2 &, const R3 &, const std::string &)> Callback_t;

    /** called by gdbus on error or completion of call */
    static void dbusCallback (GObject *src_obj, GAsyncResult *res, void *user_data)
    {
        typedef typename DBusClientCall<Callback_t>::CallbackData CallbackData;
        CallbackData *data = static_cast<CallbackData *>(user_data);

        GError *err = NULL;
        GDBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &err));
        typename dbus_traits<R1>::host_type r1;
        typename dbus_traits<R2>::host_type r2;
        typename dbus_traits<R3>::host_type r3;
        if (err != NULL) {
            ExtractArgs(data->m_conn.get(), reply.get()) >> Get<R1>(r1) >> Get<R2>(r2) >> Get<R3>(r3);
        }
        //unmarshal the return results and call user callback
        (data->m_callback)(r1, r2, r3, err->message);
    }

public:
    DBusClientCall3 (const DBusCallObject &object)
        : DBusClientCall<Callback_t>(object, &DBusClientCall3::dbusCallback)
    {
    }

    DBusClientCall3 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Callback_t>(object, method, &DBusClientCall3::dbusCallback)
    {
    }
};

/**
 * Common functionality of all SignalWatch* classes.
 * @param T     boost::function with the right signature
 */
template <class T> class SignalWatch
{
 public:
    SignalWatch(const DBusRemoteObject &object,
                 const std::string &signal)
        : m_object(object), m_signal(signal)
    {
    }

    ~SignalWatch()
    {
        if (m_tag) {
            g_dbus_connection_signal_unsubscribe(m_object.getConnection(), m_tag);
        }
    }

    typedef T Callback_t;
    const Callback_t &getCallback() const{ return m_callback; }

 protected:
    const DBusRemoteObject &m_object;
    std::string m_signal;
    guint m_tag;
    T m_callback;

    static gboolean isMatched(GDBusMessage *msg, void *data)
    {
        SignalWatch *watch = static_cast<SignalWatch*>(data);
        return boost::iequals(g_dbus_message_get_path(msg), watch->m_object.getPath()) &&
            g_dbus_message_get_message_type(msg) == G_DBUS_MESSAGE_TYPE_SIGNAL;
    }

    void activateInternal(const Callback_t &callback, GDBusSignalCallback cb)
    {
        m_callback = callback;
        m_tag = g_dbus_connection_signal_subscribe(m_object.getConnection(),
                                                   NULL,
                                                   m_object.getInterface(),
                                                   m_signal.c_str(),
                                                   m_object.getPath(),
                                                   NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   cb,
                                                   this,
                                                   NULL);
    }
};

class SignalWatch0 : public SignalWatch< boost::function<void (void)> >
{
    typedef boost::function<void (void)> Callback_t;

 public:
    SignalWatch0(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();
        cb();
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (void)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1>
class SignalWatch1 : public SignalWatch< boost::function<void (const A1 &)> >
{
    typedef boost::function<void (const A1 &)> Callback_t;

 public:
    SignalWatch1(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();

        typename dbus_traits<A1>::host_type a1;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, iter, a1);
        cb(a1);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1, typename A2>
class SignalWatch2 : public SignalWatch< boost::function<void (const A1 &, const A2 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &)> Callback_t;

 public:
    SignalWatch2(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();

        typename dbus_traits<A1>::host_type a1;
        typename dbus_traits<A2>::host_type a2;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, iter, a1);
        dbus_traits<A2>::get(conn, NULL, iter, a2);
        cb(a1, a2);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

template <typename A1, typename A2, typename A3>
class SignalWatch3 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &)> Callback_t;

 public:
    SignalWatch3(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        /* if (SignalWatch<Callback_t>::isMatched(msg, data) == FALSE) { */
        /*     return TRUE; */
        /* } */
        const Callback_t &cb =static_cast< SignalWatch<Callback_t> *>(data)->getCallback();

        typename dbus_traits<A1>::host_type a1;
        typename dbus_traits<A2>::host_type a2;
        typename dbus_traits<A3>::host_type a3;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, iter, a1);
        dbus_traits<A2>::get(conn, NULL, iter, a2);
        dbus_traits<A3>::get(conn, NULL, iter, a3);
        cb(a1, a2, a3);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &,
                                           const A2 &,
                                           const A3 &)> >::activateInternal(callback,
                                                                            internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4>
class SignalWatch4 : public SignalWatch< boost::function<void (const A1 &, const A2 &,
                                                               const A3 &, const A4 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &)> Callback_t;

 public:
    SignalWatch4(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();


        typename dbus_traits<A1>::host_type a1;
        typename dbus_traits<A2>::host_type a2;
        typename dbus_traits<A3>::host_type a3;
        typename dbus_traits<A4>::host_type a4;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, &iter, a1);
        dbus_traits<A2>::get(conn, NULL, &iter, a2);
        dbus_traits<A3>::get(conn, NULL, &iter, a3);
        dbus_traits<A4>::get(conn, NULL, &iter, a4);
        cb(a1, a2, a3, a4);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5>
class SignalWatch5 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &, const A5 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &, const A5 &)> Callback_t;

 public:
    SignalWatch5(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();


        typename dbus_traits<A1>::host_type a1;
        typename dbus_traits<A2>::host_type a2;
        typename dbus_traits<A3>::host_type a3;
        typename dbus_traits<A4>::host_type a4;
        typename dbus_traits<A5>::host_type a5;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, iter, a1);
        dbus_traits<A2>::get(conn, NULL, iter, a2);
        dbus_traits<A3>::get(conn, NULL, iter, a3);
        dbus_traits<A4>::get(conn, NULL, iter, a4);
        dbus_traits<A5>::get(conn, NULL, iter, a5);
        cb(a1, a2, a3, a4, a5);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &,
                                           const A5 &)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
class SignalWatch6 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &,
                                                               const A4 &, const A5 &, const A6 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &,
                                  const A4 &, const A5 &, const A6 &)> Callback_t;


 public:
    SignalWatch6(const DBusRemoteObject &object,
                 const std::string &signal)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        const Callback_t &cb = static_cast< SignalWatch<Callback_t> *>(data)->getCallback();

        typename dbus_traits<A1>::host_type a1;
        typename dbus_traits<A2>::host_type a2;
        typename dbus_traits<A3>::host_type a3;
        typename dbus_traits<A4>::host_type a4;
        typename dbus_traits<A5>::host_type a5;
        typename dbus_traits<A6>::host_type a6;

        GVariantIter iter;
        g_variant_iter_init(&iter, params);
        dbus_traits<A1>::get(conn, NULL, iter, a1);
        dbus_traits<A2>::get(conn, NULL, iter, a2);
        dbus_traits<A3>::get(conn, NULL, iter, a3);
        dbus_traits<A4>::get(conn, NULL, iter, a4);
        dbus_traits<A5>::get(conn, NULL, iter, a5);
        dbus_traits<A6>::get(conn, NULL, iter, a6);
        cb(a1, a2, a3, a4, a5, a6);
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &,
                                           const A5 &, const A6 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

} // namespace GDBusCXX

#endif // INCL_GDBUS_CXX_BRIDGE
