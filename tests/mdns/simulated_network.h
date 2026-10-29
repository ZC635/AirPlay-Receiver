#pragma once
// Only OS interface enumeration and UDP transport are replaced; DNS code is real.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <QtCore>
#include <QtNetwork>

class SimNetworkInterface {
public:
    enum Flag { IsUp = 1, IsRunning = 2, CanMulticast = 4, IsLoopBack = 8 };
    int id = 0;
    QString label;
    QHostAddress address;
    int prefix = 0;
    int flags() const;
    int index() const { return id; }
    QList<QNetworkAddressEntry> addressEntries() const;
    static QList<SimNetworkInterface> allInterfaces();
};
struct SimPacket {
    QByteArray bytes;
    QHostAddress destination;
    quint16 port;
    int egress;
    QHostAddress source;
    int hopLimit = -1;
};
class SimUdpSocket;
struct SimNetwork {
    static constexpr int Lan = 12, Campus = 5;
    inline static bool campusUp = true, ipv6 = false, overlap = false, missingMetadata = false;
    inline static bool ineligibleCampus = false;
    // Deliver multicast back to every local member socket, including the
    // sender, as IP_MULTICAST_LOOP does by default.
    inline static bool loopback = false;
    inline static QString lanAddress = "172.16.176.254";
    inline static int defaultMulticastEgress = Campus;
    inline static int failJoin = 0, failSend = 0;
    inline static QList<SimPacket> sent;
    inline static QList<SimUdpSocket *> sockets;
    inline static int receiveCount = 0, selectInterfaceCalls = 0;
    static bool inject(int ingress, const QByteArray &, const QHostAddress &, quint16);
};
class SimUdpSocket final : public QUdpSocket {
    Q_OBJECT
public:
    SimUdpSocket();
    ~SimUdpSocket() override;
    bool bind(const QHostAddress &, quint16, QAbstractSocket::BindMode);
    QAbstractSocket::SocketState state() const;
    bool joinMulticastGroup(const QHostAddress &, const SimNetworkInterface &);
    bool leaveMulticastGroup(const QHostAddress &, const SimNetworkInterface &);
    void setMulticastInterface(const SimNetworkInterface &);
    void setSocketOption(QAbstractSocket::SocketOption option, const QVariant &value) override {
        if (option == QAbstractSocket::MulticastTtlOption) multicastHopLimit = value.toInt();
    }
    qintptr socketDescriptor() const override { return reinterpret_cast<qintptr>(this); }
    int multicastHopLimit = 1, unicastHopLimit = 128;
    qint64 writeDatagram(const QByteArray &, const QHostAddress &, quint16);
    qint64 writeDatagram(const QNetworkDatagram &);
    bool hasPendingDatagrams() const { return !incoming.isEmpty(); }
    QNetworkDatagram receiveDatagram(qint64 = -1);
    qint64 pendingDatagramSize() const;
    qint64 readDatagram(char *, qint64, QHostAddress *, quint16 *);
    void inject(const QByteArray &, const QHostAddress &, quint16, int);
    QSet<int> members;
    QAbstractSocket::NetworkLayerProtocol family = QAbstractSocket::UnknownNetworkLayerProtocol;
private:
    bool bound = false;
    int explicitInterface = 0;
    QQueue<QNetworkDatagram> incoming;
};
#ifdef SIMULATE_QMDNS_NETWORK
#ifdef Q_OS_WIN
inline int simSetSocketOption(SOCKET descriptor, int level, int option, const char *value, int length) {
    if (level != IPPROTO_IP || option != IP_TTL || length != sizeof(int)) return SOCKET_ERROR;
    for (auto *socket : SimNetwork::sockets) {
        if (static_cast<SOCKET>(socket->socketDescriptor()) == descriptor) {
            memcpy(&socket->unicastHopLimit, value, sizeof(int));
            return 0;
        }
    }
    return SOCKET_ERROR;
}
#define setsockopt simSetSocketOption
#endif
#define QNetworkInterface SimNetworkInterface
#define QUdpSocket SimUdpSocket
#endif
