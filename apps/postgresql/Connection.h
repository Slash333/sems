#ifndef PQ_CONNECTION_H
#define PQ_CONNECTION_H

#include <postgresql/libpq-fe.h>
#include <string>
using std::string;

class IPGConnection;
class IPGTransaction;

struct IConnectionHandler
{
    enum EventType
    {
        PG_SOCK_NEW = 0,
        PG_SOCK_DEL,
        PG_SOCK_WRITE,
        PG_SOCK_READ
    };

    virtual ~IConnectionHandler(){}
    virtual void onSock(IPGConnection* conn, EventType type) = 0;
    virtual void onConnect(IPGConnection* conn) = 0;
    virtual void onConnectionFailed(IPGConnection* conn, const string& error) = 0;
    virtual void onDisconnect(IPGConnection* conn) = 0;
    virtual void onReset(IPGConnection* conn) = 0;
    virtual void onPQError(IPGConnection* conn, const string& error) = 0;
    virtual void onStopTransaction(IPGTransaction* trans) = 0;
};

class IPGConnection
{
protected:
    string connection_info;
    IConnectionHandler* handler;
    ConnStatusType status;
    int conn_fd;
    time_t disconnected_time;
protected:
    friend class IPGTransaction;
    IPGTransaction* cur_transition;
    IPGTransaction* planned;

    virtual void check_conn() = 0;
    virtual void* get_conn() = 0;
    virtual bool reset_conn() = 0;
    virtual void close_conn() = 0;
public:
    IPGConnection(const string& conn_info, IConnectionHandler* handler)
    : connection_info(conn_info), handler(handler)
    , status(CONNECTION_BAD), conn_fd(-1)
    , cur_transition(0)
    , planned(0)
    , disconnected_time(time(0)){}
    virtual ~IPGConnection();


    void check();
    void* get() { return get_conn(); } 
    bool reset();
    void close() { return close_conn(); }
    bool runTransaction(IPGTransaction* trans);
    bool addPlannedTransaction(IPGTransaction* trans);
    void stopTransaction();
    void cancelTransaction();
    ConnStatusType getStatus() { return status; }
    int getSocket() { return conn_fd; }
    string getConnInfo() { return connection_info; }
    bool isBusy() { return cur_transition ? true : false; }
    time_t getDisconnectedTime() { return disconnected_time; } 
};

class PGConnection : public IPGConnection
{
    PGconn* conn;
    bool connected;

    bool reset_conn() override;
    void check_conn() override;
    void* get_conn() override;
    void close_conn() override;
public:
    PGConnection(const string& conn_info, IConnectionHandler* handler);
    ~PGConnection();

};

class MockConnection : public IPGConnection
{
    bool reset_conn() override;
    void check_conn() override;
    void* get_conn() override;
    void close_conn() override;
public:
    MockConnection(IConnectionHandler* handler);
    ~MockConnection();
};

#endif/*PQ_CONNECTION_H*/
