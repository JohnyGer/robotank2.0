#include "robo_core.h"

// msgs
#include "influence.h"
#include "joy_axes.h"

#include "pub_sub.h"

#include <QPointF>

#include <limits>

namespace
{
    constexpr float defaultDotsPerDegree = 100;
    constexpr double influenceCoef = 90.0 / 32767;

    enum Axes
    {
        X1 = 0,
        Y1 = 1,
        X2 = 2,
        Y2 = 5,
        DigitalX = 6,
        DigitalY = 7,
    };

    template < class T >
    inline T bound(T minValue, T value, T maxValue)
    {
        return std::min(std::max(value, minValue), maxValue);
    }
}

class RoboCore::Impl
{
public:
    State state = State::Search;
    Influence influence;
    QPointF dotsPerDegree;

    short enginePowerLeft = SHRT_MAX;
    short enginePowerRight = SHRT_MAX;
    bool hasNewData = false;
    JoyAxes joy;

    Publisher< Influence >* influenceP = nullptr;

    double smooth(double value, double maxInputValue, double maxOutputValue) const;
};

RoboCore::RoboCore():
    ITask(),
    d(new Impl)
{
    d->influenceP = PubSub::instance()->advertise< Influence >("core/influence");

    PubSub::instance()->subscribe("tracker/status", &RoboCore::onTrackerStatusChanged, this);
    PubSub::instance()->subscribe("tracker/deviation", &RoboCore::onTrackerDeviation, this);
    PubSub::instance()->subscribe("camera/dotsPerDegree", &RoboCore::onDotsPerDegreeChanged, this);
    PubSub::instance()->subscribe("core/enginePower", &RoboCore::onEnginePowerChanged, this);
    PubSub::instance()->subscribe("joy/axes", &RoboCore::onJoyEvent, this);
    PubSub::instance()->subscribe("core/shot", &RoboCore::onFireStatusChanged, this);

    this->onTrackerStatusChanged(false);
}

RoboCore::~RoboCore()
{
    delete d->influenceP;
    delete d;
}

void RoboCore::execute()
{
    if (d->hasNewData) 
    {
        d->hasNewData = false;
        d->influenceP->publish(d->influence);
//        qDebug() << Q_FUNC_INFO << d->influence.leftEngine << d->influence.rightEngine << d->influence.gunV << d->influence.towerH;
    }
}

void RoboCore::onJoyEvent(const JoyAxes& joy)
{
    if (d->state == State::Search)
    {
        if (d->joy.axes[Axes::X2] != joy.axes[Axes::X2] || d->joy.axes[Axes::Y2] != joy.axes[Axes::Y2])
        {
//            qDebug() << Q_FUNC_INFO << joy.axes;
            short x = d->smooth(joy.axes[Axes::X2], SHRT_MAX, SHRT_MAX);
            short y = d->smooth(joy.axes[Axes::Y2], SHRT_MAX, SHRT_MAX);

            // Y axis is inverted. Up is negative, down is positive
            d->influence.gunV = d->influence.cameraV = -y;
            d->influence.towerH = x;
    //        qDebug() << Q_FUNC_INFO << d->influence.gunV << d->influence.towerH << joy.axes;
            d->joy.axes[Axes::X2] = joy.axes[Axes::X2];
            d->joy.axes[Axes::Y2] = joy.axes[Axes::Y2];
            d->hasNewData = true;
        }
    }

    if (d->joy.axes[Axes::X1] != joy.axes[Axes::X1] 
        || d->joy.axes[Axes::Y1] != joy.axes[Axes::Y1]
        || d->joy.axes[Axes::DigitalX] != joy.axes[Axes::DigitalX] 
        || d->joy.axes[Axes::DigitalY] != joy.axes[Axes::DigitalY])
    {
//            qDebug() << Q_FUNC_INFO << joy.axes;
        // Y axis is inverted. Up is negative, down is positive
        const int speed = -(joy.axes[Axes::DigitalY] ? joy.axes[Axes::DigitalY] : joy.axes[Axes::Y1]);
        const int turn = joy.axes[Axes::DigitalX] ? joy.axes[Axes::DigitalX] : joy.axes[Axes::X1];

        d->influence.leftEngine = ::bound< int >(SHRT_MIN,
                                d->smooth(speed - turn, SHRT_MAX, d->enginePowerLeft), SHRT_MAX);
        d->influence.rightEngine = ::bound< int >(SHRT_MIN,
                                d->smooth(speed + turn, SHRT_MAX, d->enginePowerRight), SHRT_MAX);

        d->joy.axes[Axes::X1] = joy.axes[Axes::X1];
        d->joy.axes[Axes::Y1] = joy.axes[Axes::Y1];
        d->joy.axes[Axes::DigitalX] = joy.axes[Axes::DigitalX];
        d->joy.axes[Axes::DigitalY] = joy.axes[Axes::DigitalY];
        d->hasNewData = true;
    //    qDebug() << Q_FUNC_INFO << d->influence.leftEngine << d->influence.rightEngine << speed << turnSpeed << joy.axes;
    }
}

void RoboCore::onTrackerDeviation(const QPointF& deviation)
{
    if (d->state != State::Track) return;

    d->influence.towerH = (deviation.x() / d->dotsPerDegree.x()) / ::influenceCoef; // TODO PID
    d->influence.gunV = d->influence.cameraV =
            (deviation.y() / d->dotsPerDegree.y()) / ::influenceCoef;
    qDebug() << Q_FUNC_INFO << deviation 
             << QPointF((deviation.x() / d->dotsPerDegree.x()), (deviation.y() / d->dotsPerDegree.y()))
             << d->influence.towerH << d->influence.gunV;
    d->hasNewData = true;
}

void RoboCore::onEnginePowerChanged(const QPoint& enginePower)
{
    d->enginePowerLeft = enginePower.x() * SHRT_MAX / 100;
    d->enginePowerRight = enginePower.y() * SHRT_MAX / 100;
}

void RoboCore::onTrackerStatusChanged(const bool& status)
{
    if (status)
    {
        d->state = State::Track;
        d->influence.angleType = AngleType::Position;
    }
    else
    {
        d->state = State::Search;
        d->influence.angleType = AngleType::Velocity;
    }
    d->hasNewData = true;
}

void RoboCore::onFireStatusChanged(const bool& status)
{
    qDebug() << Q_FUNC_INFO << status;
    d->influence.shot = status;
    d->hasNewData = true;
}

void RoboCore::onDotsPerDegreeChanged(const QPointF& dpd)
{
    d->dotsPerDegree = dpd;
}

//------------------------------------------------------------------------------------
double RoboCore::Impl::smooth(double value, double maxInputValue, double maxOutputValue) const
{
    return pow((value / maxInputValue), 3) * maxOutputValue;
}
