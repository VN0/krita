/*
 *  Copyright (c) 2007 Boudewijn Rempt <boud@valdyas.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_shape_layer_canvas.h"

#include <QPainter>
#include <QMutexLocker>

#include <KoShapeManager.h>
#include <KoSelectedShapesProxySimple.h>
#include <KoViewConverter.h>
#include <KoColorSpace.h>

#include <kis_paint_device.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_painter.h>
#include <flake/kis_shape_layer.h>
#include <KoCompositeOpRegistry.h>
#include <KoSelection.h>
#include <KoUnit.h>
#include "kis_image_view_converter.h"

#include <kis_debug.h>

#include <QThread>
#include <QApplication>

#include <kis_spontaneous_job.h>
#include "kis_global.h"
#include "krita_utils.h"

KisShapeLayerCanvasBase::KisShapeLayerCanvasBase(KisShapeLayer *parent, KisImageWSP image)
    : KoCanvasBase(0)
    , m_viewConverter(new KisImageViewConverter(image))
    , m_shapeManager(new KoShapeManager(this))
    , m_selectedShapesProxy(new KoSelectedShapesProxySimple(m_shapeManager.data()))
{
    m_shapeManager->selection()->setActiveLayer(parent);
}

KoShapeManager *KisShapeLayerCanvasBase::shapeManager() const
{
    return m_shapeManager.data();
}

KoSelectedShapesProxy *KisShapeLayerCanvasBase::selectedShapesProxy() const
{
    return m_selectedShapesProxy.data();
}

KoViewConverter* KisShapeLayerCanvasBase::viewConverter() const
{
    return m_viewConverter.data();
}

void KisShapeLayerCanvasBase::gridSize(QPointF *offset, QSizeF *spacing) const
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(false); // This should never be called as this canvas should have no tools.
    Q_UNUSED(offset);
    Q_UNUSED(spacing);
}

bool KisShapeLayerCanvasBase::snapToGrid() const
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(false); // This should never be called as this canvas should have no tools.
    return false;
}

void KisShapeLayerCanvasBase::addCommand(KUndo2Command *)
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(false); // This should never be called as this canvas should have no tools.
}


KoToolProxy * KisShapeLayerCanvasBase::toolProxy() const
{
//     KIS_SAFE_ASSERT_RECOVER_NOOP(false); // This should never be called as this canvas should have no tools.
    return 0;
}

QWidget* KisShapeLayerCanvasBase::canvasWidget()
{
    return 0;
}

const QWidget* KisShapeLayerCanvasBase::canvasWidget() const
{
    return 0;
}

KoUnit KisShapeLayerCanvasBase::unit() const
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(false); // This should never be called as this canvas should have no tools.
    return KoUnit(KoUnit::Point);
}

void KisShapeLayerCanvasBase::prepareForDestroying()
{
    m_isDestroying = true;
}

bool KisShapeLayerCanvasBase::hasChangedWhileBeingInvisible()
{
    return m_hasChangedWhileBeingInvisible;
}


KisShapeLayerCanvas::KisShapeLayerCanvas(KisShapeLayer *parent, KisImageWSP image)
        : KisShapeLayerCanvasBase(parent, image)
        , m_projection(0)
        , m_parentLayer(parent)
        , m_asyncUpdateSignalCompressor(100, KisSignalCompressor::FIRST_INACTIVE)
{
    /**
     * The layour should also add itself to its own shape manager, so that the canvas
     * would track its changes/transformations
     */
    m_shapeManager->addShape(parent, KoShapeManager::AddWithoutRepaint);
    m_shapeManager->selection()->setActiveLayer(parent);

    connect(&m_asyncUpdateSignalCompressor, SIGNAL(timeout()), SLOT(slotStartAsyncRepaint()));

    setImage(image);
}

KisShapeLayerCanvas::~KisShapeLayerCanvas()
{
    m_shapeManager->remove(m_parentLayer);
}

void KisShapeLayerCanvas::setImage(KisImageWSP image)
{
    if (m_image) {
        disconnect(m_image, 0, this, 0);
    }

    m_viewConverter->setImage(image);
    m_image = image;

    if (image) {
        connect(m_image, SIGNAL(sigSizeChanged(QPointF,QPointF)), SLOT(slotImageSizeChanged()));
        m_cachedImageRect = m_image->bounds();
    }
}

class KisRepaintShapeLayerLayerJob : public KisSpontaneousJob
{
public:
    KisRepaintShapeLayerLayerJob(KisShapeLayerSP layer, KisShapeLayerCanvas *canvas)
        : m_layer(layer),
          m_canvas(canvas)
    {
    }

    bool overrides(const KisSpontaneousJob *_otherJob) override {
        const KisRepaintShapeLayerLayerJob *otherJob =
            dynamic_cast<const KisRepaintShapeLayerLayerJob*>(_otherJob);

        return otherJob && otherJob->m_canvas == m_canvas;
    }

    void run() override {
        m_canvas->repaint();
    }

    int levelOfDetail() const override {
        return 0;
    }

    QString debugName() const override {
        QString result;
        QDebug dbg(&result);
        dbg << "KisRepaintShapeLayerLayerJob" << m_layer;
        return result;
    }

private:

    // we store a pointer to the layer just
    // to keep the lifetime of the canvas!
    KisShapeLayerSP m_layer;

    KisShapeLayerCanvas *m_canvas;
};


void KisShapeLayerCanvas::updateCanvas(const QVector<QRectF> &region)
{
    if (!m_parentLayer->image() || m_isDestroying) {
        return;
    }

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        Q_FOREACH (const QRectF &rc, region) {
            // grow for antialiasing
            const QRect imageRect = kisGrowRect(m_viewConverter->documentToView(rc).toAlignedRect(), 2);
            m_dirtyRegion += imageRect;
        }
    }

    m_asyncUpdateSignalCompressor.start();
    m_hasUpdateInCompressor = true;
}


void KisShapeLayerCanvas::updateCanvas(const QRectF& rc)
{
    updateCanvas(QVector<QRectF>({rc}));
}

void KisShapeLayerCanvas::slotStartAsyncRepaint()
{
    QRect repaintRect;
    bool forceUpdateHiddenAreasOnly = false;
    const qint32 MASK_IMAGE_WIDTH = 256;
    const qint32 MASK_IMAGE_HEIGHT = 256;
    {
        QMutexLocker locker(&m_dirtyRegionMutex);

        repaintRect = m_dirtyRegion.boundingRect();
        forceUpdateHiddenAreasOnly = m_forceUpdateHiddenAreasOnly;

        /// Since we are going to override the previous jobs, we should fetch
        /// all the area covered by it. Otherwise we'll get dirty leftovers of
        /// the layer on the projection
        Q_FOREACH (const KoShapeManager::PaintJob &job, m_paintJobs) {
            repaintRect |= m_viewConverter->documentToView().mapRect(job.docUpdateRect).toAlignedRect();
        }
        m_paintJobs.clear();

        m_dirtyRegion = QRegion();
        m_forceUpdateHiddenAreasOnly = false;
    }

    if (!forceUpdateHiddenAreasOnly) {
        if (repaintRect.isEmpty()) {
            return;
        }

        // Crop the update rect by the image bounds. We keep the cache consistent
        // by tracking the size of the image in slotImageSizeChanged()
        repaintRect = repaintRect.intersected(m_parentLayer->image()->bounds());
    } else {
        const QRectF shapesBounds = KoShape::boundingRect(m_shapeManager->shapes());
        repaintRect = kisGrowRect(m_viewConverter->documentToView(shapesBounds).toAlignedRect(), 2);
    }

    /**
     * Vector shapes are not thread-safe against concurrent read-writes, so we
     * need to utilize rather complicated policy on accessing them:
     *
     * 1) All shape writes happen in GUI thread (right in the tools)
     * 2) No concurrent reads from the shapes may happen  in other threads
     *    while the user is modifying them.
     *
     * That is why our shape rendering code is split into two parts:
     *
     * 1) First we just fetch a shallow copy of the shapes of the layer (it
     *    takes about 1ms for complicated vecotor layers) and pack them into
     *    KoShapeManager::PaintJobsList jobs. It happens here, in
     *    slotStartAsyncRepaint(), which runs in the GUI thread. It guarantees
     *    that noone is accessing the shapes during the copy operation.
     *
     * 2) The rendering itself happens in the worker thread in repaint(). But
     *    repaint() doesn't access original shapes anymore. It accesses only they
     *    shallow copies, which means that there is no concurrent
     *    access to anything (*).
     *
     * (*) "no concurrent access to anything" is a rather fragile term :) There
     *     will still be concurrent access to it, on detaching... But(!), when detaching,
     *     the original data is kept unchanged, so "it should be safe enough"(c). Especially
     *     if we guarantee that rendering thread may not cause a detach (?), and the detach
     *     can happen only from a single GUI thread.
     */

    const QVector<QRect> updateRects =
        KritaUtils::splitRectIntoPatchesTight(repaintRect,
                                              QSize(MASK_IMAGE_WIDTH, MASK_IMAGE_HEIGHT));

    KoShapeManager::PaintJobsList jobs;
    Q_FOREACH (const QRect &viewUpdateRect, updateRects) {
        jobs << KoShapeManager::PaintJob(m_viewConverter->viewToDocument().mapRect(QRectF(viewUpdateRect)),
                                         viewUpdateRect);
    }

    m_shapeManager->preparePaintJobs(jobs, m_parentLayer);

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        m_paintJobs = jobs;
    }

    m_hasUpdateInCompressor = false;
    m_image->addSpontaneousJob(new KisRepaintShapeLayerLayerJob(m_parentLayer, this));
}

void KisShapeLayerCanvas::slotImageSizeChanged()
{
    QRegion dirtyCacheRegion;
    dirtyCacheRegion += m_image->bounds();
    dirtyCacheRegion += m_cachedImageRect;
    dirtyCacheRegion -= m_image->bounds() & m_cachedImageRect;

    QVector<QRectF> dirtyRects;
    auto rc = dirtyCacheRegion.begin();
    while (rc != dirtyCacheRegion.end()) {
        dirtyRects.append(m_viewConverter->viewToDocument(*rc));
        rc++;
    }
    updateCanvas(dirtyRects);

    m_cachedImageRect = m_image->bounds();
}

void KisShapeLayerCanvas::repaint()
{

    KoShapeManager::PaintJobsList paintJobs;

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        std::swap(paintJobs, m_paintJobs);
    }

    const qint32 MASK_IMAGE_WIDTH = 256;
    const qint32 MASK_IMAGE_HEIGHT = 256;

    QImage image(MASK_IMAGE_WIDTH, MASK_IMAGE_HEIGHT, QImage::Format_ARGB32);
    QPainter tempPainter(&image);

    tempPainter.setRenderHint(QPainter::Antialiasing);
    tempPainter.setRenderHint(QPainter::TextAntialiasing);

    quint8 * dstData = new quint8[MASK_IMAGE_WIDTH * MASK_IMAGE_HEIGHT * m_projection->pixelSize()];

    QRect repaintRect;

    Q_FOREACH (const KoShapeManager::PaintJob &job, paintJobs) {
        image.fill(0);

        tempPainter.setTransform(QTransform());
        tempPainter.setClipRect(QRect(0,0,MASK_IMAGE_WIDTH,MASK_IMAGE_HEIGHT));
        tempPainter.setTransform(m_viewConverter->documentToView() *
                                 QTransform::fromTranslate(-job.viewUpdateRect.x(), -job.viewUpdateRect.y()));

        m_shapeManager->paintJob(tempPainter, job, false);

        KoColorSpaceRegistry::instance()->rgb8()
        ->convertPixelsTo(image.constBits(), dstData, m_projection->colorSpace(),
                          MASK_IMAGE_WIDTH * MASK_IMAGE_HEIGHT,
                          KoColorConversionTransformation::internalRenderingIntent(),
                          KoColorConversionTransformation::internalConversionFlags());

        // TODO: use job.viewUpdateRect instead of MASK_IMAGE_WIDTH/HEIGHT
        m_projection->writeBytes(dstData,
                                 job.viewUpdateRect.x(),
                                 job.viewUpdateRect.y(),
                                 MASK_IMAGE_WIDTH,
                                 MASK_IMAGE_HEIGHT);

        repaintRect |= job.viewUpdateRect;
    }

    delete[] dstData;
    m_projection->purgeDefaultPixels();
    m_parentLayer->setDirty(repaintRect);

    m_hasChangedWhileBeingInvisible |= !m_parentLayer->visible(true);
}

void KisShapeLayerCanvas::forceRepaint()
{
    /**
     * WARNING! Although forceRepaint() may be called from different threads, it is
     * not entirely safe. If the user plays with shapes at the same time (vector tools are
     * not ported to strokes yet), the shapes my be accessed from two different places at
     * the same time, which will cause a crash.
     *
     * The only real solution to this is to port vector tools to strokes framework.
     */

    if (hasPendingUpdates()) {
        m_asyncUpdateSignalCompressor.stop();
        slotStartAsyncRepaint();
    }
}

bool KisShapeLayerCanvas::hasPendingUpdates() const
{
    return m_hasUpdateInCompressor;
}

void KisShapeLayerCanvas::forceRepaintWithHiddenAreas()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_parentLayer->image());
    KIS_SAFE_ASSERT_RECOVER_RETURN(!m_isDestroying);

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        m_forceUpdateHiddenAreasOnly = true;
    }

    m_asyncUpdateSignalCompressor.stop();
    slotStartAsyncRepaint();
}

void KisShapeLayerCanvas::resetCache()
{
    m_projection->clear();

    QList<KoShape*> shapes = m_shapeManager->shapes();
    Q_FOREACH (const KoShape* shape, shapes) {
        shape->update();
    }
}

void KisShapeLayerCanvas::rerenderAfterBeingInvisible()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_parentLayer->visible(true));

    m_hasChangedWhileBeingInvisible = false;
    resetCache();
}
