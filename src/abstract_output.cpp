/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2018 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "abstract_output.h"
#include <KSharedConfig>
#include <KConfigGroup>

namespace KWin
{

QDebug operator<<(QDebug debug, const AbstractOutput *output)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    if (output) {
        debug << output->metaObject()->className() << '(' << static_cast<const void *>(output);
        debug << ", name=" << output->name();
        debug << ", geometry=" << output->geometry();
        debug << ", scale=" << output->scale();
        if (debug.verbosity() > 2) {
            debug << ", manufacturer=" << output->manufacturer();
            debug << ", model=" << output->model();
            debug << ", serialNumber=" << output->serialNumber();
        }
        debug << ')';
    } else {
        debug << "AbstractOutput(0x0)";
    }
    return debug;
}

AbstractOutput::AbstractOutput(QObject *parent)
    : QObject(parent)
{
}

AbstractOutput::~AbstractOutput()
{
}

QUuid AbstractOutput::uuid() const
{
    return QUuid();
}

bool AbstractOutput::isEnabled() const
{
    return true;
}

void AbstractOutput::setEnabled(bool enable)
{
    Q_UNUSED(enable)
}

bool AbstractOutput::isInternal() const
{
    return false;
}

qreal AbstractOutput::scale() const
{
    return 1;
}

QSize AbstractOutput::physicalSize() const
{
    return QSize();
}

QString AbstractOutput::manufacturer() const
{
    return QString();
}

QString AbstractOutput::model() const
{
    return QString();
}

QString AbstractOutput::serialNumber() const
{
    return QString();
}

RenderLoop *AbstractOutput::renderLoop() const
{
    return nullptr;
}

void AbstractOutput::inhibitDirectScanout()
{
    m_directScanoutCount++;
}
void AbstractOutput::uninhibitDirectScanout()
{
    m_directScanoutCount--;
}

bool AbstractOutput::directScanoutInhibited() const
{
    return m_directScanoutCount;
}

std::chrono::milliseconds AbstractOutput::dimAnimationTime()
{
    // See kscreen.kcfg
    return std::chrono::milliseconds (KSharedConfig::openConfig()->group("Effect-Kscreen").readEntry("Duration", 250));
}

bool AbstractOutput::usesSoftwareCursor() const
{
    return true;
}

void AbstractOutput::setColorTransformation(const QSharedPointer<ColorTransformation> &transformation)
{
    Q_UNUSED(transformation);
}

} // namespace KWin
