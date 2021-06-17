/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "qpaintersurfacetextureprovider_wayland.h"
#include "krknativetexture.h"
#include "krktexture.h"
#include "surfaceitem_wayland.h"

#include <KWaylandServer/shmclientbuffer.h>
#include <KWaylandServer/surface_interface.h>

#include <QPainter>

namespace KWin
{

QPainterSurfaceTextureProviderWayland::QPainterSurfaceTextureProviderWayland(QPainterBackend *backend,
                                                                             SurfacePixmapWayland *pixmap)
    : QPainterSurfaceTextureProvider(backend)
    , m_pixmap(pixmap)
{
}

bool QPainterSurfaceTextureProviderWayland::create()
{
    auto buffer = qobject_cast<KWaylandServer::ShmClientBuffer *>(m_pixmap->buffer());
    if (Q_LIKELY(buffer)) {
        // The buffer data is copied as the buffer interface returns a QImage
        // which doesn't own the data of the underlying wl_shm_buffer object.
        m_image = buffer->data().copy();
    }
    if (!m_image.isNull()) {
        m_sceneTexture.reset(KrkNative::KrkSoftwareTexture::fromNative(m_image));
    }
    return !m_image.isNull();
}

void QPainterSurfaceTextureProviderWayland::update(const QRegion &region)
{
    auto buffer = qobject_cast<KWaylandServer::ShmClientBuffer *>(m_pixmap->buffer());
    if (Q_UNLIKELY(!buffer)) {
        return;
    }

    const QImage image = buffer->data();
    const QRegion dirtyRegion = mapRegion(m_pixmap->item()->surfaceToBufferMatrix(), region);
    QPainter painter(&m_image);

    // The buffer data is copied as the buffer interface returns a QImage
    // which doesn't own the data of the underlying wl_shm_buffer object.
    for (const QRect &rect : dirtyRegion) {
        painter.drawImage(rect, image, rect);
    }

    m_sceneTexture.reset(KrkNative::KrkSoftwareTexture::fromNative(m_image));
}

} // namespace KWin
