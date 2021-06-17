/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "basiceglsurfacetextureprovider_internal.h"
#include "krknativetexture.h"
#include "krktexture.h"
#include "kwingltexture.h"
#include "logging.h"
#include "surfaceitem_internal.h"

#include <QOpenGLFramebufferObject>

namespace KWin
{

BasicEGLSurfaceTextureProviderInternal::BasicEGLSurfaceTextureProviderInternal(
        OpenGLBackend *backend,
        SurfacePixmapInternal *pixmap)
    : OpenGLSurfaceTextureProvider(backend)
    , m_pixmap(pixmap)
{
}

bool BasicEGLSurfaceTextureProviderInternal::create()
{
    if (updateFromFramebuffer()) {
        return true;
    } else if (updateFromImage(m_pixmap->image().rect())) {
        return true;
    } else {
        qCDebug(KWIN_OPENGL) << "Failed to create surface texture for internal window";
        return false;
    }
}

void BasicEGLSurfaceTextureProviderInternal::update(const QRegion &region)
{
    if (updateFromFramebuffer()) {
        return;
    } else if (updateFromImage(region)) {
        return;
    } else {
        qCDebug(KWIN_OPENGL) << "Failed to update surface texture for internal window";
    }
}

bool BasicEGLSurfaceTextureProviderInternal::updateFromFramebuffer()
{
    const QOpenGLFramebufferObject *fbo = m_pixmap->fbo();
    if (!fbo) {
        return false;
    }
    m_texture.reset(new GLTexture(fbo->texture(), 0, fbo->size()));
    m_texture->setWrapMode(GL_CLAMP_TO_EDGE);
    m_texture->setFilter(GL_LINEAR);
    m_texture->setYInverted(false);

    KrkNative::KrkNativeTexture::CreateTextureOptions options;
    if (m_pixmap->hasAlphaChannel()) {
        options |= KrkNative::KrkNativeTexture::TextureHasAlpha;
    }
    m_sceneTexture.reset(KrkNative::KrkOpenGLTexture::fromNative(m_texture.data(), options));
    return true;
}

static QRegion scale(const QRegion &region, qreal scaleFactor)
{
    if (scaleFactor == 1) {
        return region;
    }

    QRegion scaled;
    for (const QRect &rect : region) {
        scaled += QRect(rect.topLeft() * scaleFactor, rect.size() * scaleFactor);
    }
    return scaled;
}

bool BasicEGLSurfaceTextureProviderInternal::updateFromImage(const QRegion &region)
{
    const QImage image = m_pixmap->image();
    if (image.isNull()) {
        return false;
    }

    if (!m_texture) {
        m_texture.reset(new GLTexture(image));
        KrkNative::KrkNativeTexture::CreateTextureOptions options;
        if (image.hasAlphaChannel()) {
            options |= KrkNative::KrkNativeTexture::TextureHasAlpha;
        }
        m_sceneTexture.reset(KrkNative::KrkOpenGLTexture::fromNative(m_texture.data(), options));
    } else {
        const QRegion nativeRegion = scale(region, image.devicePixelRatio());
        for (const QRect &rect : nativeRegion) {
            m_texture->update(image, rect.topLeft(), rect);
        }
    }

    return true;
}

} // namespace KWin
