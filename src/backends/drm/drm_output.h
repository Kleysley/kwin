/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "drm_abstract_output.h"
#include "drm_object.h"
#include "drm_plane.h"

#include <QObject>
#include <QPoint>
#include <QSize>
#include <QTimer>
#include <QVector>
#include <chrono>
#include <xf86drmMode.h>

namespace KWin
{

class DrmConnector;
class DrmGpu;
class DrmPipeline;
class DumbSwapchain;
class GLTexture;
class RenderTarget;
class DrmLease;

class KWIN_EXPORT DrmOutput : public DrmAbstractOutput
{
    Q_OBJECT
public:
    DrmOutput(DrmPipeline *pipeline);
    ~DrmOutput() override;

    DrmConnector *connector() const;
    DrmPipeline *pipeline() const;

    bool present() override;
    DrmOutputLayer *primaryLayer() const override;

    bool queueChanges(const OutputConfiguration &config);
    void applyQueuedChanges(const OutputConfiguration &config);
    void revertQueuedChanges();
    void updateModes();
    void updateDpmsMode(DpmsMode dpmsMode);

    bool setCursor(const QImage &image, const QPoint &hotspot) override;
    bool moveCursor(const QPoint &position) override;
    void resetCursorTexture();

    DrmLease *lease() const;
    bool addLeaseObjects(QVector<uint32_t> &objectList);
    void leased(DrmLease *lease);
    void leaseEnded();

    void setColorTransformation(const std::shared_ptr<ColorTransformation> &transformation) override;

private:
    bool setDrmDpmsMode(DpmsMode mode);
    void setDpmsMode(DpmsMode mode) override;

    QList<std::shared_ptr<OutputMode>> getModes() const;

    void renderCursorOpengl(const RenderTarget &renderTarget, const QSize &cursorSize);
    void renderCursorQPainter(const RenderTarget &renderTarget);

    DrmPipeline *m_pipeline;
    DrmConnector *m_connector;

    bool m_setCursorSuccessful = false;
    bool m_moveCursorSuccessful = false;
    QTimer m_turnOffTimer;
    DrmLease *m_lease = nullptr;

    struct {
        QImage image;
        QPoint hotspot;
        QPoint position;

        std::unique_ptr<GLTexture> texture;
        qint64 cacheKey = 0;
    } m_cursor;
};

}

Q_DECLARE_METATYPE(KWin::DrmOutput *)
