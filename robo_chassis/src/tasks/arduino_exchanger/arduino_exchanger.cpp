#include "arduino_exchanger.h"

//msgs
#include "influence.h"
#include "pointf3d.h"
#include "empty.h"

#include "pub_sub.h"

#include <QPoint>
#include <QSerialPort>
#include <QElapsedTimer>
#include <QTimer>
#include <QDebug>

namespace
{
#pragma pack(push, 1)
    struct ArduinoPkg
    {
        int16_t yaw = 0;
        int16_t pitch = 0;
        int16_t roll = 0;
    };
    struct RaspberryPkg
    {
        RaspberryPkg(){}

        uint8_t shot = 0;
        int16_t leftEngine = 0;
        int16_t rightEngine = 0;
        int16_t towerH = 0;
    };
#pragma pack(pop)

    const int timeout = 500; // ms
    const int sendInterval = 50; // ms
    constexpr double positionCoef = 360.0 / 32767;
} // namespace

class ArduinoExchanger::Impl
{
public:
    QSerialPort* serial = nullptr;
    QTimer* timer = nullptr;
    QTimer* sendTimer = nullptr;

    QByteArray buffer;
    QByteArray prefix = QByteArray(2, 0x55);
    bool waitPrefix = true;

    ArduinoPkg offsets;
    ArduinoPkg lastData;
    RaspberryPkg package;

    Publisher< PointF3D >* yprP = nullptr;
    Publisher< bool >* arduinoStatusP = nullptr;

    void sendData();
    void readData();
    void setArduinoOnline(bool set);
    bool isArduinoOnline() const;

private:
    bool arduinoOnline = true;
};

ArduinoExchanger::ArduinoExchanger():
    ITask(),
    d(new Impl)
{
    d->serial = new QSerialPort("/dev/ttyAMA0", this);
    d->serial->setBaudRate(115200);
    d->serial->setDataBits(QSerialPort::Data8);
    d->serial->setParity(QSerialPort::NoParity);
    d->serial->setStopBits(QSerialPort::OneStop);
    connect(d->serial, &QSerialPort::readyRead, this, [&](){ d->readData(); });

    d->timer = new QTimer(this);
    d->timer->setInterval(::timeout);
    connect(d->timer, &QTimer::timeout, this, [&](){ d->setArduinoOnline(false); });

    d->sendTimer = new QTimer(this);
    d->sendTimer->setInterval(::sendInterval);
    connect(d->sendTimer, &QTimer::timeout, this, [&](){ d->sendData(); });

    d->yprP = PubSub::instance()->advertise< PointF3D >("chassis/ypr");
    d->arduinoStatusP = PubSub::instance()->advertise< bool >("arduino/status");

    PubSub::instance()->subscribe("core/influence", &ArduinoExchanger::onInfluence, this);

    d->setArduinoOnline(false);
    d->package.shot = 0;
}

ArduinoExchanger::~ArduinoExchanger()
{
    if (d->serial->isOpen()) d->serial->close();
    delete d->yprP;
    delete d->arduinoStatusP;
    delete d;
}

void ArduinoExchanger::execute()
{
    if (!d->serial->isOpen()) d->serial->open(QIODevice::ReadWrite);
}

void ArduinoExchanger::start()
{
    d->serial->open(QIODevice::ReadWrite);
    d->sendTimer->start();
}

void ArduinoExchanger::onInfluence(const Influence& influence)
{
    if (!d->isArduinoOnline()) return;
    d->package.shot = influence.shot;
    d->package.leftEngine = influence.leftEngine;
    d->package.rightEngine = influence.rightEngine;
    d->package.towerH = influence.towerH;
}

//------------------------------------------------------------------------------------

void ArduinoExchanger::Impl::readData()
{
    QByteArray data = serial->readAll();
    if (serial->error() != QSerialPort::NoError) return;

    buffer.append(data);
    if (waitPrefix)
    {
        if (buffer.size() < prefix.size()) return;

        auto idx = buffer.indexOf(prefix);
        if (idx == -1)
        {
            buffer.remove(0, buffer.size() - prefix.size());
            return;
        }

        buffer.remove(0, idx);
        waitPrefix = false;
    }

    if (!waitPrefix)
    {
        const int packetSize = prefix.size() + sizeof(::ArduinoPkg);
        if (buffer.size() < packetSize) return;

        timer->start();
        this->setArduinoOnline(true);

        ::ArduinoPkg pkg = *reinterpret_cast<::ArduinoPkg *>(
                    buffer.mid(prefix.size(), sizeof(::ArduinoPkg)).data());
//        qDebug() << Q_FUNC_INFO << pkg.ax << pkg.ay << pkg.az << buffer.mid(prefix.size(), sizeof(::ArduinoPkg)).toHex();

        buffer.remove(0, packetSize);
        waitPrefix = true;

        yprP->publish(PointF3D({pkg.yaw * ::positionCoef, pkg.pitch * ::positionCoef, pkg.roll * ::positionCoef}));
    }
}

void ArduinoExchanger::Impl::setArduinoOnline(bool online)
{
    if (online == arduinoOnline) return;
    qDebug() << (online ? "Arduino online" : "Arduino offline");
    arduinoOnline = online;
    arduinoStatusP->publish(online);
}

void ArduinoExchanger::Impl::sendData()
{
    if (!serial->isOpen()) return;
    if (!isArduinoOnline()) return;

//    qDebug() << Q_FUNC_INFO << package.leftEngine << package.rightEngine << package.towerH;

    QByteArray data(prefix);
    data.append(QByteArray(reinterpret_cast<const char *>(&package), sizeof(package)));

    if (serial->write(data) != data.size())
    {
        qDebug() << Q_FUNC_INFO << "Failed to write to the bus.";
    }
}

bool ArduinoExchanger::Impl::isArduinoOnline() const
{
    return arduinoOnline;
}