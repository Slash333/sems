#include "AmMediaSecureUdptlState.h"

AmMediaSecureUdptlState::AmMediaSecureUdptlState(AmMediaTransport *transport)
  : AmMediaState(transport)
{
}

AmMediaState* AmMediaSecureUdptlState::init(const AmArg& args)
{
    string address = args["address"].asCStr();
    int port = args["port"].asInt();
    vector<AmStreamConnection *> new_conns;
    transport->iterateConnections(AmStreamConnection::DTLS_CONN, [&](auto conn, bool& stop) {
        CLASS_DBG("add dtls-udptl connection, state:%s, type:%s, raddr:%s, rport:%d",
                  state2str(), transport->type2str(), address.c_str(), port);
        new_conns.push_back(transport->getConnFactory()->createDtlsUdptlConnection(address, port, conn));
    });

    transport->addConnections(new_conns);
    transport->setMode(AmMediaTransport::TRANSPORT_MODE_DTLS_FAX);
    return this;
}

const char* AmMediaSecureUdptlState::state2str()
{
    static const char *state = "DTLS-UDPTL";
    return state;
}
