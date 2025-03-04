/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_output.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_connector.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_pipeline.h"

#include "core/outputconfiguration.h"
#include "core/renderloop.h"
#include "core/renderloop_p.h"
#include "core/session.h"
#include "drm_dumb_buffer.h"
#include "drm_dumb_swapchain.h"
#include "drm_egl_backend.h"
#include "drm_layer.h"
#include "drm_logging.h"
#include "kwinglutils.h"
// Qt
#include <QCryptographicHash>
#include <QMatrix4x4>
#include <QPainter>
// c++
#include <cerrno>
// drm
#include <drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>

namespace KWin
{

DrmOutput::DrmOutput(DrmPipeline *pipeline)
    : DrmAbstractOutput(pipeline->connector()->gpu())
    , m_pipeline(pipeline)
    , m_connector(pipeline->connector())
{
    RenderLoopPrivate::get(m_renderLoop.get())->canDoTearing = gpu()->asyncPageflipSupported();
    m_pipeline->setOutput(this);
    const auto conn = m_pipeline->connector();
    m_renderLoop->setRefreshRate(m_pipeline->mode()->refreshRate());

    Capabilities capabilities = Capability::Dpms;
    State initialState;

    if (conn->hasOverscan()) {
        capabilities |= Capability::Overscan;
        initialState.overscan = conn->overscan();
    }
    if (conn->vrrCapable()) {
        capabilities |= Capability::Vrr;
        setVrrPolicy(RenderLoop::VrrPolicy::Automatic);
    }
    if (conn->hasRgbRange()) {
        capabilities |= Capability::RgbRange;
        initialState.rgbRange = conn->rgbRange();
    }

    const Edid *edid = conn->edid();

    setInformation(Information{
        .name = conn->connectorName(),
        .manufacturer = edid->manufacturerString(),
        .model = conn->modelName(),
        .serialNumber = edid->serialNumber(),
        .eisaId = edid->eisaId(),
        .physicalSize = conn->physicalSize(),
        .edid = edid->raw(),
        .subPixel = conn->subpixel(),
        .capabilities = capabilities,
        .panelOrientation = DrmConnector::toKWinTransform(conn->panelOrientation()),
        .internal = conn->isInternal(),
        .nonDesktop = conn->isNonDesktop(),
    });

    initialState.modes = getModes();
    initialState.currentMode = m_pipeline->mode();
    if (!initialState.currentMode) {
        initialState.currentMode = initialState.modes.constFirst();
    }

    setState(initialState);

    m_turnOffTimer.setSingleShot(true);
    m_turnOffTimer.setInterval(dimAnimationTime());
    connect(&m_turnOffTimer, &QTimer::timeout, this, [this] {
        setDrmDpmsMode(DpmsMode::Off);
    });
}

DrmOutput::~DrmOutput()
{
    m_pipeline->setOutput(nullptr);
}

bool DrmOutput::addLeaseObjects(QVector<uint32_t> &objectList)
{
    if (!m_pipeline->crtc()) {
        qCWarning(KWIN_DRM) << "Can't lease connector: No suitable crtc available";
        return false;
    }
    qCDebug(KWIN_DRM) << "adding connector" << m_pipeline->connector()->id() << "to lease";
    objectList << m_pipeline->connector()->id();
    objectList << m_pipeline->crtc()->id();
    if (m_pipeline->crtc()->primaryPlane()) {
        objectList << m_pipeline->crtc()->primaryPlane()->id();
    }
    return true;
}

void DrmOutput::leased(DrmLease *lease)
{
    m_lease = lease;
}

void DrmOutput::leaseEnded()
{
    qCDebug(KWIN_DRM) << "ended lease for connector" << m_pipeline->connector()->id();
    m_lease = nullptr;
}

DrmLease *DrmOutput::lease() const
{
    return m_lease;
}

bool DrmOutput::setCursor(const QImage &image, const QPoint &hotspot)
{
    static bool valid;
    static const bool forceSoftwareCursor = qEnvironmentVariableIntValue("KWIN_FORCE_SW_CURSOR", &valid) == 1 && valid;
    // hardware cursors are broken with the NVidia proprietary driver
    if (forceSoftwareCursor || (!valid && m_gpu->isNVidia())) {
        m_setCursorSuccessful = false;
        return false;
    }
    const auto layer = m_pipeline->cursorLayer();
    if (!m_pipeline->crtc() || !layer) {
        return false;
    }
    m_cursor.image = image;
    m_cursor.hotspot = hotspot;
    if (m_cursor.image.isNull()) {
        if (layer->isVisible()) {
            layer->setVisible(false);
            m_pipeline->setCursor();
        }
        return true;
    }
    bool rendered = false;
    const QMatrix4x4 monitorMatrix = logicalToNativeMatrix(rect(), scale(), transform());
    const QSize cursorSize = m_cursor.image.size() / m_cursor.image.devicePixelRatio();
    const QRect cursorRect = QRect(m_cursor.position, cursorSize);
    const QRect nativeCursorRect = monitorMatrix.mapRect(cursorRect);
    if (nativeCursorRect.width() <= m_gpu->cursorSize().width() && nativeCursorRect.height() <= m_gpu->cursorSize().height()) {
        if (const auto beginInfo = layer->beginFrame()) {
            const auto &[renderTarget, repaint] = beginInfo.value();
            if (dynamic_cast<EglGbmBackend *>(m_gpu->platform()->renderBackend())) {
                renderCursorOpengl(renderTarget, cursorSize * scale());
            } else {
                renderCursorQPainter(renderTarget);
            }
            rendered = layer->endFrame(infiniteRegion(), infiniteRegion());
        }
    }
    if (!rendered) {
        if (layer->isVisible()) {
            layer->setVisible(false);
            m_pipeline->setCursor();
        }
        m_setCursorSuccessful = false;
        return false;
    }

    const QSize layerSize = m_gpu->cursorSize() / scale();
    const QRect layerRect = monitorMatrix.mapRect(QRect(m_cursor.position, layerSize));
    layer->setVisible(cursorRect.intersects(rect()));
    if (layer->isVisible()) {
        m_setCursorSuccessful = m_pipeline->setCursor(logicalToNativeMatrix(QRect(QPoint(), layerRect.size()), scale(), transform()).map(m_cursor.hotspot));
        layer->setVisible(m_setCursorSuccessful);
    }
    return m_setCursorSuccessful;
}

bool DrmOutput::moveCursor(const QPoint &position)
{
    if (!m_setCursorSuccessful || !m_pipeline->crtc()) {
        return false;
    }
    m_cursor.position = position;

    const QSize cursorSize = m_cursor.image.size() / m_cursor.image.devicePixelRatio();
    const QRect cursorRect = QRect(m_cursor.position, cursorSize);

    if (m_cursor.image.isNull() || !cursorRect.intersects(rect())) {
        const auto layer = m_pipeline->cursorLayer();
        if (layer->isVisible()) {
            layer->setVisible(false);
            m_pipeline->setCursor();
        }
        return true;
    }
    const QMatrix4x4 monitorMatrix = logicalToNativeMatrix(rect(), scale(), transform());
    const QSize layerSize = m_gpu->cursorSize() / scale();
    const QRect layerRect = monitorMatrix.mapRect(QRect(m_cursor.position, layerSize));
    const auto layer = m_pipeline->cursorLayer();
    layer->setVisible(true);
    layer->setPosition(layerRect.topLeft());
    m_moveCursorSuccessful = m_pipeline->moveCursor();
    layer->setVisible(m_moveCursorSuccessful);
    if (!m_moveCursorSuccessful) {
        m_pipeline->setCursor();
    }
    return m_moveCursorSuccessful;
}

void DrmOutput::resetCursorTexture()
{
    m_cursor.texture.reset();
}

QList<std::shared_ptr<OutputMode>> DrmOutput::getModes() const
{
    const auto drmModes = m_pipeline->connector()->modes();

    QList<std::shared_ptr<OutputMode>> ret;
    ret.reserve(drmModes.count());
    for (const auto &drmMode : drmModes) {
        ret.append(drmMode);
    }
    return ret;
}

void DrmOutput::setDpmsMode(DpmsMode mode)
{
    if (mode == DpmsMode::Off) {
        if (!m_turnOffTimer.isActive()) {
            Q_EMIT aboutToTurnOff(std::chrono::milliseconds(m_turnOffTimer.interval()));
            m_turnOffTimer.start();
        }
        if (isEnabled()) {
            m_gpu->platform()->createDpmsFilter();
        }
    } else {
        m_gpu->platform()->checkOutputsAreOn();
        if (m_turnOffTimer.isActive() || (mode != dpmsMode() && setDrmDpmsMode(mode))) {
            Q_EMIT wakeUp();
        }
        m_turnOffTimer.stop();
    }
}

bool DrmOutput::setDrmDpmsMode(DpmsMode mode)
{
    if (!isEnabled()) {
        return false;
    }
    bool active = mode == DpmsMode::On;
    bool isActive = dpmsMode() == DpmsMode::On;
    if (active == isActive) {
        updateDpmsMode(mode);
        return true;
    }
    m_pipeline->setActive(active);
    if (DrmPipeline::commitPipelines({m_pipeline}, active ? DrmPipeline::CommitMode::TestAllowModeset : DrmPipeline::CommitMode::CommitModeset) == DrmPipeline::Error::None) {
        m_pipeline->applyPendingChanges();
        updateDpmsMode(mode);
        if (active) {
            m_gpu->platform()->checkOutputsAreOn();
            m_renderLoop->uninhibit();
            m_renderLoop->scheduleRepaint();
        } else {
            m_renderLoop->inhibit();
            m_gpu->platform()->createDpmsFilter();
        }
        return true;
    } else {
        qCWarning(KWIN_DRM) << "Setting dpms mode failed!";
        m_pipeline->revertPendingChanges();
        if (isEnabled() && isActive && !active) {
            m_gpu->platform()->checkOutputsAreOn();
        }
        return false;
    }
}

DrmPlane::Transformations outputToPlaneTransform(DrmOutput::Transform transform)
{
    using OutTrans = DrmOutput::Transform;
    using PlaneTrans = DrmPlane::Transformation;

    // TODO: Do we want to support reflections (flips)?

    switch (transform) {
    case OutTrans::Normal:
    case OutTrans::Flipped:
        return PlaneTrans::Rotate0;
    case OutTrans::Rotated90:
    case OutTrans::Flipped90:
        return PlaneTrans::Rotate90;
    case OutTrans::Rotated180:
    case OutTrans::Flipped180:
        return PlaneTrans::Rotate180;
    case OutTrans::Rotated270:
    case OutTrans::Flipped270:
        return PlaneTrans::Rotate270;
    default:
        Q_UNREACHABLE();
    }
}

void DrmOutput::updateModes()
{
    State next = m_state;
    next.modes = getModes();

    if (m_pipeline->crtc()) {
        const auto currentMode = m_pipeline->connector()->findMode(m_pipeline->crtc()->queryCurrentMode());
        if (currentMode != m_pipeline->mode()) {
            // DrmConnector::findCurrentMode might fail
            m_pipeline->setMode(currentMode ? currentMode : m_pipeline->connector()->modes().constFirst());
            if (m_gpu->testPendingConfiguration() == DrmPipeline::Error::None) {
                m_pipeline->applyPendingChanges();
                m_renderLoop->setRefreshRate(m_pipeline->mode()->refreshRate());
            } else {
                qCWarning(KWIN_DRM) << "Setting changed mode failed!";
                m_pipeline->revertPendingChanges();
            }
        }
    }

    next.currentMode = m_pipeline->mode();
    if (!next.currentMode) {
        next.currentMode = next.modes.constFirst();
    }

    setState(next);
}

void DrmOutput::updateDpmsMode(DpmsMode dpmsMode)
{
    State next = m_state;
    next.dpmsMode = dpmsMode;
    setState(next);
}

bool DrmOutput::present()
{
    RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(m_renderLoop.get());
    const auto type = DrmConnector::kwinToDrmContentType(contentType());
    if (m_pipeline->syncMode() != renderLoopPrivate->presentMode || type != m_pipeline->contentType()) {
        m_pipeline->setSyncMode(renderLoopPrivate->presentMode);
        m_pipeline->setContentType(type);
        if (DrmPipeline::commitPipelines({m_pipeline}, DrmPipeline::CommitMode::Test) == DrmPipeline::Error::None) {
            m_pipeline->applyPendingChanges();
        } else {
            m_pipeline->revertPendingChanges();
        }
    }
    const bool needsModeset = gpu()->needsModeset();
    bool success;
    if (needsModeset) {
        success = m_pipeline->maybeModeset();
    } else {
        DrmPipeline::Error err = m_pipeline->present();
        success = err == DrmPipeline::Error::None;
        if (err == DrmPipeline::Error::InvalidArguments) {
            QTimer::singleShot(0, m_gpu->platform(), &DrmBackend::updateOutputs);
        }
    }
    if (success) {
        Q_EMIT outputChange(m_pipeline->primaryLayer()->currentDamage());
        return true;
    } else if (!needsModeset) {
        qCWarning(KWIN_DRM) << "Presentation failed!" << strerror(errno);
        frameFailed();
    }
    return false;
}

DrmConnector *DrmOutput::connector() const
{
    return m_connector;
}

DrmPipeline *DrmOutput::pipeline() const
{
    return m_pipeline;
}

bool DrmOutput::queueChanges(const OutputConfiguration &config)
{
    static bool valid;
    static int envOnlySoftwareRotations = qEnvironmentVariableIntValue("KWIN_DRM_SW_ROTATIONS_ONLY", &valid) == 1 || !valid;

    const auto props = config.constChangeSet(this);
    const auto mode = props->mode.lock();
    if (!mode) {
        return false;
    }
    m_pipeline->setMode(std::static_pointer_cast<DrmConnectorMode>(mode));
    m_pipeline->setOverscan(props->overscan);
    m_pipeline->setRgbRange(props->rgbRange);
    m_pipeline->setRenderOrientation(outputToPlaneTransform(props->transform));
    if (!envOnlySoftwareRotations && m_gpu->atomicModeSetting()) {
        m_pipeline->setBufferOrientation(m_pipeline->renderOrientation());
    }
    m_pipeline->setEnable(props->enabled);
    return true;
}

void DrmOutput::applyQueuedChanges(const OutputConfiguration &config)
{
    if (!m_connector->isConnected()) {
        return;
    }
    Q_EMIT aboutToChange();
    m_pipeline->applyPendingChanges();

    auto props = config.constChangeSet(this);

    State next = m_state;
    next.enabled = props->enabled && m_pipeline->crtc();
    next.position = props->pos;
    next.scale = props->scale;
    next.transform = props->transform;
    next.currentMode = m_pipeline->mode();
    next.overscan = m_pipeline->overscan();
    next.rgbRange = m_pipeline->rgbRange();

    setState(next);
    setVrrPolicy(props->vrrPolicy);

    if (!isEnabled() && m_pipeline->needsModeset()) {
        m_gpu->maybeModeset();
    }

    m_renderLoop->setRefreshRate(refreshRate());
    m_renderLoop->scheduleRepaint();

    Q_EMIT changed();

    if (isEnabled() && dpmsMode() == DpmsMode::On) {
        m_gpu->platform()->turnOutputsOn();
    }
}

void DrmOutput::revertQueuedChanges()
{
    m_pipeline->revertPendingChanges();
}

DrmOutputLayer *DrmOutput::primaryLayer() const
{
    return m_pipeline->primaryLayer();
}

void DrmOutput::setColorTransformation(const std::shared_ptr<ColorTransformation> &transformation)
{
    m_pipeline->setColorTransformation(transformation);
    if (DrmPipeline::commitPipelines({m_pipeline}, DrmPipeline::CommitMode::Test) == DrmPipeline::Error::None) {
        m_pipeline->applyPendingChanges();
        m_renderLoop->scheduleRepaint();
    } else {
        m_pipeline->revertPendingChanges();
    }
}

void DrmOutput::renderCursorOpengl(const RenderTarget &renderTarget, const QSize &cursorSize)
{
    auto allocateTexture = [this]() {
        if (m_cursor.image.isNull()) {
            m_cursor.texture.reset();
            m_cursor.cacheKey = 0;
        } else {
            m_cursor.texture = std::make_unique<GLTexture>(m_cursor.image);
            m_cursor.texture->setWrapMode(GL_CLAMP_TO_EDGE);
            m_cursor.cacheKey = m_cursor.image.cacheKey();
        }
    };

    if (!m_cursor.texture) {
        allocateTexture();
    } else if (m_cursor.cacheKey != m_cursor.image.cacheKey()) {
        if (m_cursor.image.size() == m_cursor.texture->size()) {
            m_cursor.texture->update(m_cursor.image);
            m_cursor.cacheKey = m_cursor.image.cacheKey();
        } else {
            allocateTexture();
        }
    }

    QMatrix4x4 mvp;
    mvp.ortho(QRect(QPoint(), renderTarget.size()));

    GLFramebuffer *fbo = std::get<GLFramebuffer *>(renderTarget.nativeHandle());
    GLFramebuffer::pushFramebuffer(fbo);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_cursor.texture->bind();
    ShaderBinder binder(ShaderTrait::MapTexture);
    binder.shader()->setUniform(GLShader::ModelViewProjectionMatrix, mvp);
    m_cursor.texture->render(QRect(0, 0, cursorSize.width(), cursorSize.height()), renderTarget.devicePixelRatio());
    m_cursor.texture->unbind();
    glDisable(GL_BLEND);

    GLFramebuffer::popFramebuffer();
}

void DrmOutput::renderCursorQPainter(const RenderTarget &renderTarget)
{
    const QRect cursorRect(QPoint(0, 0), m_cursor.image.size() / m_cursor.image.devicePixelRatio());

    QImage *c = std::get<QImage *>(renderTarget.nativeHandle());
    c->setDevicePixelRatio(scale());
    c->fill(Qt::transparent);

    QPainter p;
    p.begin(c);
    p.setWorldTransform(logicalToNativeMatrix(cursorRect, 1, transform()).toTransform());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QPoint(0, 0), m_cursor.image);
    p.end();
}
}
