/****************************************************************************
**
** Copyright (C) 2014 Jolla Ltd, author: <gunnar.sletta@jollamobile.com>
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Quick 2D Renderer module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "threadedrenderloop.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtCore/QAnimationDriver>
#include <QtCore/QQueue>
#include <QtCore/QTime>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QOffscreenSurface>

#include <qpa/qwindowsysteminterface.h>
#include <qpa/qplatformbackingstore.h>

#include <QtQuick/QQuickWindow>
#include <private/qquickwindow_p.h>

#include <QtQuick/private/qsgrenderer_p.h>

#include <private/qquickanimatorcontroller_p.h>

#include <private/qquickprofiler_p.h>
#include <private/qqmldebugserviceinterfaces_p.h>
#include <private/qqmldebugconnector_p.h>
#include "context.h"

/*
   Overall design:

   There are two classes here. ThreadedRenderLoop and
   RenderThread. All communication between the two is based on
   event passing and we have a number of custom events.

   In this implementation, the render thread is never blocked and the
   GUI thread will initiate a polishAndSync which will block and wait
   for the render thread to pick it up and release the block only
   after the render thread is done syncing. The reason for this
   is:

   1. Clear blocking paradigm. We only have one real "block" point
   (polishAndSync()) and all blocking is initiated by GUI and picked
   up by Render at specific times based on events. This makes the
   execution deterministic.

   2. Render does not have to interact with GUI. This is done so that
   the render thread can run its own animation system which stays
   alive even when the GUI thread is blocked doing i/o, object
   instantiation, QPainter-painting or any other non-trivial task.

   ---

   There is one thread per window and one opengl context per thread.

   ---

   The render thread has affinity to the GUI thread until a window
   is shown. From that moment and until the window is destroyed, it
   will have affinity to the render thread. (moved back at the end
   of run for cleanup).

   ---

   The render loop is active while any window is exposed. All visible
   windows are tracked, but only exposed windows are actually added to
   the render thread and rendered. That means that if all windows are
   obscured, we might end up cleaning up the SG and GL context (if all
   windows have disabled persistency). Especially for multiprocess,
   low-end systems, this should be quite important.

 */

QT_BEGIN_NAMESPACE

#define QSG_RT_PAD "                    (RT)"

static inline int qsgrl_animation_interval() {
    qreal refreshRate = QGuiApplication::primaryScreen()->refreshRate();
    // To work around that some platforms wrongfully return 0 or something
    // bogus for refreshrate
    if (refreshRate < 1)
        return 16;
    return int(1000 / refreshRate);
}


static QElapsedTimer threadTimer;
static qint64 syncTime;
static qint64 renderTime;
static qint64 sinceLastTime;

extern Q_GUI_EXPORT QImage qt_gl_read_framebuffer(const QSize &size, bool alpha_format, bool include_alpha);

// RL: Render Loop
// RT: Render Thread

// Passed from the RL to the RT when a window is removed obscured and
// should be removed from the render loop.
const QEvent::Type WM_Obscure           = QEvent::Type(QEvent::User + 1);

// Passed from the RL to RT when GUI has been locked, waiting for sync
// (updatePaintNode())
const QEvent::Type WM_RequestSync       = QEvent::Type(QEvent::User + 2);

// Passed by the RT to itself to trigger another render pass. This is
// typically a result of QQuickWindow::update().
const QEvent::Type WM_RequestRepaint    = QEvent::Type(QEvent::User + 3);

// Passed by the RL to the RT to free up maybe release SG and GL contexts
// if no windows are rendering.
const QEvent::Type WM_TryRelease        = QEvent::Type(QEvent::User + 4);

// Passed by the RL to the RT when a QQuickWindow::grabWindow() is
// called.
const QEvent::Type WM_Grab              = QEvent::Type(QEvent::User + 5);

// Passed by the window when there is a render job to run
const QEvent::Type WM_PostJob           = QEvent::Type(QEvent::User + 6);

template <typename T> T *windowFor(const QList<T> &list, QQuickWindow *window)
{
    for (int i=0; i<list.size(); ++i) {
        const T &t = list.at(i);
        if (t.window == window)
            return const_cast<T *>(&t);
    }
    return 0;
}


class WMWindowEvent : public QEvent
{
public:
    WMWindowEvent(QQuickWindow *c, QEvent::Type type) : QEvent(type), window(c) { }
    QQuickWindow *window;
};

class WMTryReleaseEvent : public WMWindowEvent
{
public:
    WMTryReleaseEvent(QQuickWindow *win, bool destroy, QOffscreenSurface *fallback)
        : WMWindowEvent(win, WM_TryRelease)
        , inDestructor(destroy)
        , fallbackSurface(fallback)
    {}

    bool inDestructor;
    QOffscreenSurface *fallbackSurface;
};

class WMSyncEvent : public WMWindowEvent
{
public:
    WMSyncEvent(QQuickWindow *c, bool inExpose, bool force)
        : WMWindowEvent(c, WM_RequestSync)
        , size(c->size())
        , syncInExpose(inExpose)
        , forceRenderPass(force)
    {}
    QSize size;
    bool syncInExpose;
    bool forceRenderPass;
};


class WMGrabEvent : public WMWindowEvent
{
public:
    WMGrabEvent(QQuickWindow *c, QImage *result) : WMWindowEvent(c, WM_Grab), image(result) {}
    QImage *image;
};

class WMJobEvent : public WMWindowEvent
{
public:
    WMJobEvent(QQuickWindow *c, QRunnable *postedJob)
        : WMWindowEvent(c, WM_PostJob), job(postedJob) {}
    ~WMJobEvent() { delete job; }
    QRunnable *job;
};

class RenderThreadEventQueue : public QQueue<QEvent *>
{
public:
    RenderThreadEventQueue()
        : waiting(false)
    {
    }

    void addEvent(QEvent *e) {
        mutex.lock();
        enqueue(e);
        if (waiting)
            condition.wakeOne();
        mutex.unlock();
    }

    QEvent *takeEvent(bool wait) {
        mutex.lock();
        if (size() == 0 && wait) {
            waiting = true;
            condition.wait(&mutex);
            waiting = false;
        }
        QEvent *e = dequeue();
        mutex.unlock();
        return e;
    }

    bool hasMoreEvents() {
        mutex.lock();
        bool has = !isEmpty();
        mutex.unlock();
        return has;
    }

private:
    QMutex mutex;
    QWaitCondition condition;
    bool waiting;
};


class RenderThread : public QThread
{
    Q_OBJECT
public:
    RenderThread(ThreadedRenderLoop *w, QSGRenderContext *renderContext)
        : wm(w)
        , sgrc(renderContext)
        , animatorDriver(0)
        , pendingUpdate(0)
        , sleeping(false)
        , syncResultedInChanges(false)
        , active(false)
        , window(0)
        , stopEventProcessing(false)
    {
#if defined(Q_OS_QNX) && !defined(Q_OS_BLACKBERRY) && defined(Q_PROCESSOR_X86)
        // The SDP 6.6.0 x86 MESA driver requires a larger stack than the default.
        setStackSize(1024 * 1024);
#endif
        vsyncDelta = qsgrl_animation_interval();
    }

    ~RenderThread()
    {
        delete sgrc;
    }

    bool event(QEvent *);
    void run();

    void syncAndRender();
    void sync(bool inExpose);

    void requestRepaint()
    {
        if (sleeping)
            stopEventProcessing = true;
        if (window)
            pendingUpdate |= RepaintRequest;
    }

    void processEventsAndWaitForMore();
    void processEvents();
    void postEvent(QEvent *e);

public slots:
    void sceneGraphChanged() {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "sceneGraphChanged";
        syncResultedInChanges = true;
    }

public:
    enum UpdateRequest {
        SyncRequest         = 0x01,
        RepaintRequest      = 0x02,
        ExposeRequest       = 0x04 | RepaintRequest | SyncRequest
    };

    ThreadedRenderLoop *wm;
    QSGRenderContext *sgrc;

    QAnimationDriver *animatorDriver;

    uint pendingUpdate;
    bool sleeping;
    bool syncResultedInChanges;

    volatile bool active;

    float vsyncDelta;

    QMutex mutex;
    QWaitCondition waitCondition;

    QElapsedTimer m_timer;

    QQuickWindow *window; // Will be 0 when window is not exposed
    QSize windowSize;

    // Local event queue stuff...
    bool stopEventProcessing;
    RenderThreadEventQueue eventQueue;
};

bool RenderThread::event(QEvent *e)
{
    switch ((int) e->type()) {

    case WM_Obscure: {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_Obscure";

        Q_ASSERT(!window || window == static_cast<WMWindowEvent *>(e)->window);

        mutex.lock();
        if (window) {
            QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
            d->fireAboutToStop();
            d->cleanupNodesOnShutdown();

            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- window removed";
            window = 0;
        }
        waitCondition.wakeOne();
        mutex.unlock();

        return true; }

    case WM_RequestSync: {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_RequestSync";
        WMSyncEvent *se = static_cast<WMSyncEvent *>(e);
        if (sleeping)
            stopEventProcessing = true;
        window = se->window;
        windowSize = se->size;

        pendingUpdate |= SyncRequest;
        if (se->syncInExpose) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- triggered from expose";
            pendingUpdate |= ExposeRequest;
        }
        if (se->forceRenderPass) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- repaint regardless";
            pendingUpdate |= RepaintRequest;
        }
        return true; }

    case WM_TryRelease: {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_TryRelease";
        mutex.lock();
        wm->m_lockedForSync = true;
        WMTryReleaseEvent *wme = static_cast<WMTryReleaseEvent *>(e);
        if (!window || wme->inDestructor) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- setting exit flag and invalidating OpenGL";
            active = false;
            Q_ASSERT_X(!wme->inDestructor || !active, "RenderThread::invalidateOpenGL()", "Thread's active state is not set to false when shutting down");
            if (sleeping)
                stopEventProcessing = true;
        } else {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- not releasing because window is still active";
        }
        waitCondition.wakeOne();
        wm->m_lockedForSync = false;
        mutex.unlock();
        return true;
    }

    case WM_Grab: {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_Grab";
        WMGrabEvent *ce = static_cast<WMGrabEvent *>(e);
        Q_ASSERT(ce->window);
        Q_ASSERT(ce->window == window || !window);
        mutex.lock();
        if (window) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- sync scene graph";
            QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
            static_cast<SoftwareContext::RenderContext*>(d->context)->currentWindow = window;
            d->syncSceneGraph();

            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- rendering scene graph";
            QQuickWindowPrivate::get(window)->renderSceneGraph(windowSize);

            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- grabbing result";
            *ce->image = static_cast<SoftwareContext::Renderer*>(d->renderer)->backingStore()->handle()->toImage();
        }
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- waking gui to handle result";
        waitCondition.wakeOne();
        mutex.unlock();
        return true;
    }

    case WM_PostJob: {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_PostJob";
        WMJobEvent *ce = static_cast<WMJobEvent *>(e);
        Q_ASSERT(ce->window == window);
        if (window) {
            ce->job->run();
            delete ce->job;
            ce->job = 0;
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- job done";
        }
        return true;
    }

    case WM_RequestRepaint:
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "WM_RequestPaint";
        // When GUI posts this event, it is followed by a polishAndSync, so we mustn't
        // exit the event loop yet.
        pendingUpdate |= RepaintRequest;
        break;

    default:
        break;
    }
    return QThread::event(e);
}

/*!
    Enters the mutex lock to make sure GUI is blocking and performs
    sync, then wakes GUI.
 */
void RenderThread::sync(bool inExpose)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "sync()";
    mutex.lock();

    Q_ASSERT_X(wm->m_lockedForSync, "RenderThread::sync()", "sync triggered on bad terms as gui is not already locked...");

    bool current = false;
    if (windowSize.width() > 0 && windowSize.height() > 0)
        current = true;
    if (current) {
        QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
        static_cast<SoftwareContext::RenderContext*>(d->context)->currentWindow = window;
        bool hadRenderer = d->renderer != 0;
        // If the scene graph was touched since the last sync() make sure it sends the
        // changed signal.
        if (d->renderer)
            d->renderer->clearChangedFlag();
        d->syncSceneGraph();
        if (!hadRenderer && d->renderer) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- renderer was created";
            syncResultedInChanges = true;
            connect(d->renderer, SIGNAL(sceneGraphChanged()), this, SLOT(sceneGraphChanged()), Qt::DirectConnection);
        }

        // Process deferred deletes now, directly after the sync as
        // deleteLater on the GUI must now also have resulted in SG changes
        // and the delete is a safe operation.
        QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
    } else {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- window has bad size, sync aborted";
    }

    if (!inExpose) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- sync complete, waking Gui";
        waitCondition.wakeOne();
        mutex.unlock();
    }
}

void RenderThread::syncAndRender()
{
    bool profileFrames = QSG_RASTER_LOG_TIME_RENDERLOOP().isDebugEnabled();
    if (profileFrames) {
        sinceLastTime = threadTimer.nsecsElapsed();
        threadTimer.start();
    }
    Q_QUICK_SG_PROFILE_START(QQuickProfiler::SceneGraphRenderLoopFrame);

    QElapsedTimer waitTimer;
    waitTimer.start();

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "syncAndRender()";

    syncResultedInChanges = false;
    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);

    bool repaintRequested = (pendingUpdate & RepaintRequest) || d->customRenderStage;
    bool syncRequested = pendingUpdate & SyncRequest;
    bool exposeRequested = (pendingUpdate & ExposeRequest) == ExposeRequest;
    pendingUpdate = 0;

    if (syncRequested) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- updatePending, doing sync";
        sync(exposeRequested);
    }
#ifndef QSG_NO_RENDER_TIMING
    if (profileFrames)
        syncTime = threadTimer.nsecsElapsed();
#endif
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphRenderLoopFrame);

    if (!syncResultedInChanges && !repaintRequested) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- no changes, render aborted";
        int waitTime = vsyncDelta - (int) waitTimer.elapsed();
        if (waitTime > 0)
            msleep(waitTime);
        return;
    }

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- rendering started";


    if (animatorDriver->isRunning()) {
        d->animationController->lock();
        animatorDriver->advance();
        d->animationController->unlock();
    }

    bool current = false;
    if (d->renderer && windowSize.width() > 0 && windowSize.height() > 0)
        current = true;
    if (current) {
        static_cast<SoftwareContext::RenderContext*>(d->context)->currentWindow = window;
        d->renderSceneGraph(windowSize);
        if (profileFrames)
            renderTime = threadTimer.nsecsElapsed();
        // ### used to be swappBuffers here
        d->fireFrameSwapped();
    } else {
        Q_QUICK_SG_PROFILE_SKIP(QQuickProfiler::SceneGraphRenderLoopFrame, 1);
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- window not ready, skipping render";
    }

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- rendering done";

    // Though it would be more correct to put this block directly after
    // fireFrameSwapped in the if (current) branch above, we don't do
    // that to avoid blocking the GUI thread in the case where it
    // has started rendering with a bad window, causing makeCurrent to
    // fail or if the window has a bad size.
    if (exposeRequested) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "- wake Gui after initial expose";
        waitCondition.wakeOne();
        mutex.unlock();
    }

    qCDebug(QSG_RASTER_LOG_TIME_RENDERLOOP,
            "Frame rendered with 'threaded' renderloop in %dms, sync=%d, render=%d, swap=%d - (on render thread)",
            int(threadTimer.elapsed()),
            int((syncTime/1000000)),
            int((renderTime - syncTime) / 1000000),
            int(threadTimer.elapsed() - renderTime / 1000000));


    Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphRenderLoopFrame);
}



void RenderThread::postEvent(QEvent *e)
{
    eventQueue.addEvent(e);
}



void RenderThread::processEvents()
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "--- begin processEvents()";
    while (eventQueue.hasMoreEvents()) {
        QEvent *e = eventQueue.takeEvent(false);
        event(e);
        delete e;
    }
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "--- done processEvents()";
}

void RenderThread::processEventsAndWaitForMore()
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "--- begin processEventsAndWaitForMore()";
    stopEventProcessing = false;
    while (!stopEventProcessing) {
        QEvent *e = eventQueue.takeEvent(true);
        event(e);
        delete e;
    }
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "--- done processEventsAndWaitForMore()";
}

void RenderThread::run()
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "run()";
    animatorDriver = sgrc->sceneGraphContext()->createAnimationDriver(0);
    animatorDriver->install();
    if (QQmlDebugConnector::service<QQmlProfilerService>())
        QQuickProfiler::registerAnimationCallback();

    while (active) {

        if (window) {
            static_cast<SoftwareContext::RenderContext*>(sgrc)->initializeIfNeeded();
            syncAndRender();
        }

        processEvents();
        QCoreApplication::processEvents();

        if (active && (pendingUpdate == 0 || !window)) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "done drawing, sleep...";
            sleeping = true;
            processEventsAndWaitForMore();
            sleeping = false;
        }
    }

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << QSG_RT_PAD << "run() completed";

    delete animatorDriver;
    animatorDriver = 0;

    sgrc->moveToThread(wm->thread());
    moveToThread(wm->thread());
}

ThreadedRenderLoop::ThreadedRenderLoop()
    : sg(QSGContext::createDefaultContext())
    , m_animation_timer(0)
{
#if defined(QSG_RENDER_LOOP_DEBUG)
    qsgrl_timer.start();
#endif

    m_animation_driver = sg->createAnimationDriver(this);

    connect(m_animation_driver, SIGNAL(started()), this, SLOT(animationStarted()));
    connect(m_animation_driver, SIGNAL(stopped()), this, SLOT(animationStopped()));

    m_animation_driver->install();
}

QSGRenderContext *ThreadedRenderLoop::createRenderContext(QSGContext *sg) const
{
    return sg->createRenderContext();
}

void ThreadedRenderLoop::maybePostPolishRequest(Window *w)
{
    w->window->requestUpdate();
}

QAnimationDriver *ThreadedRenderLoop::animationDriver() const
{
    return m_animation_driver;
}

QSGContext *ThreadedRenderLoop::sceneGraphContext() const
{
    return sg;
}

bool ThreadedRenderLoop::anyoneShowing() const
{
    for (int i=0; i<m_windows.size(); ++i) {
        QQuickWindow *c = m_windows.at(i).window;
        if (c->isVisible() && c->isExposed())
            return true;
    }
    return false;
}

bool ThreadedRenderLoop::interleaveIncubation() const
{
    return m_animation_driver->isRunning() && anyoneShowing();
}

void ThreadedRenderLoop::animationStarted()
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- animationStarted()";
    startOrStopAnimationTimer();

    for (int i=0; i<m_windows.size(); ++i)
        maybePostPolishRequest(const_cast<Window *>(&m_windows.at(i)));
}

void ThreadedRenderLoop::animationStopped()
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- animationStopped()";
    startOrStopAnimationTimer();
}


void ThreadedRenderLoop::startOrStopAnimationTimer()
{
    int exposedWindows = 0;
    const Window *theOne = 0;
    for (int i=0; i<m_windows.size(); ++i) {
        const Window &w = m_windows.at(i);
        if (w.window->isVisible() && w.window->isExposed()) {
            ++exposedWindows;
            theOne = &w;
        }
    }

    if (m_animation_timer != 0 && (exposedWindows == 1 || !m_animation_driver->isRunning())) {
        killTimer(m_animation_timer);
        m_animation_timer = 0;
        // If animations are running, make sure we keep on animating
        if (m_animation_driver->isRunning())
            maybePostPolishRequest(const_cast<Window *>(theOne));

    } else if (m_animation_timer == 0 && exposedWindows != 1 && m_animation_driver->isRunning()) {
        m_animation_timer = startTimer(qsgrl_animation_interval());
    }
}

/*
    Removes this window from the list of tracked windowes in this
    window manager. hide() will trigger obscure, which in turn will
    stop rendering.

    This function will be called during QWindow::close() which will
    also destroy the QPlatformWindow so it is important that this
    triggers handleObscurity() and that rendering for that window
    is fully done and over with by the time this function exits.
 */

void ThreadedRenderLoop::hide(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "hide()" << window;

    if (window->isExposed())
        handleObscurity(windowFor(m_windows, window));

    releaseResources(window);
}


/*!
    If the window is first hide it, then perform a complete cleanup
    with releaseResources which will take down the GL context and
    exit the rendering thread.
 */
void ThreadedRenderLoop::windowDestroyed(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "begin windowDestroyed()" << window;

    Window *w = windowFor(m_windows, window);
    if (!w)
        return;

    handleObscurity(w);
    releaseResources(w, true);

    RenderThread *thread = w->thread;
    while (thread->isRunning())
        QThread::yieldCurrentThread();
    Q_ASSERT(thread->thread() == QThread::currentThread());
    delete thread;

    for (int i=0; i<m_windows.size(); ++i) {
        if (m_windows.at(i).window == window) {
            m_windows.removeAt(i);
            break;
        }
    }

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "done windowDestroyed()" << window;
}


void ThreadedRenderLoop::exposureChanged(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "exposureChanged()" << window;
    if (window->isExposed()) {
        handleExposure(window);
    } else {
        Window *w = windowFor(m_windows, window);
        if (w)
            handleObscurity(w);
    }
}

/*!
    Will post an event to the render thread that this window should
    start to render.
 */
void ThreadedRenderLoop::handleExposure(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "handleExposure()" << window;

    Window *w = windowFor(m_windows, window);
    if (!w) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- adding window to list";
        Window win;
        win.window = window;
        win.actualWindowFormat = window->format();
        win.thread = new RenderThread(this, QQuickWindowPrivate::get(window)->context);
        win.updateDuringSync = false;
        win.forceRenderPass = true; // also covered by polishAndSync(inExpose=true), but doesn't hurt
        m_windows << win;
        w = &m_windows.last();
    }

    // set this early as we'll be rendering shortly anyway and this avoids
    // specialcasing exposure in polishAndSync.
    w->thread->window = window;

    if (w->window->width() <= 0 || w->window->height() <= 0
        || (w->window->isTopLevel() && !w->window->geometry().intersects(w->window->screen()->availableGeometry()))) {
#ifndef QT_NO_DEBUG
        qWarning().noquote().nospace() << "ThreadedRenderLoop: expose event received for window "
            << w->window << " with invalid geometry: " << w->window->geometry()
            << " on " << w->window->screen();
#endif
    }

    // Because we are going to bind a GL context to it, make sure it
    // is created.
    if (!w->window->handle())
        w->window->create();

    // Start render thread if it is not running
    if (!w->thread->isRunning()) {

        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- starting render thread";

        QQuickAnimatorController *controller = QQuickWindowPrivate::get(w->window)->animationController;
        if (controller->thread() != w->thread)
            controller->moveToThread(w->thread);

        w->thread->active = true;
        if (w->thread->thread() == QThread::currentThread()) {
            w->thread->sgrc->moveToThread(w->thread);
            w->thread->moveToThread(w->thread);
        }
        w->thread->start();
        if (!w->thread->isRunning())
            qFatal("Render thread failed to start, aborting application.");

    } else {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- render thread already running";
    }

    polishAndSync(w, true);
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- done with handleExposure()";

    startOrStopAnimationTimer();
}

/*!
    This function posts an event to the render thread to remove the window
    from the list of windowses to render.

    It also starts up the non-vsync animation tick if no more windows
    are showing.
 */
void ThreadedRenderLoop::handleObscurity(Window *w)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "handleObscurity()" << w->window;
    if (w->thread->isRunning()) {
        w->thread->mutex.lock();
        w->thread->postEvent(new WMWindowEvent(w->window, WM_Obscure));
        w->thread->waitCondition.wait(&w->thread->mutex);
        w->thread->mutex.unlock();
    }
    startOrStopAnimationTimer();
}


void ThreadedRenderLoop::handleUpdateRequest(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- polish and sync update request";
    Window *w = windowFor(m_windows, window);
    if (w)
        polishAndSync(w);
}

void ThreadedRenderLoop::maybeUpdate(QQuickWindow *window)
{
    Window *w = windowFor(m_windows, window);
    if (w)
        maybeUpdate(w);
}

/*!
    Called whenever the QML scene has changed. Will post an event to
    ourselves that a sync is needed.
 */
void ThreadedRenderLoop::maybeUpdate(Window *w)
{
    if (!QCoreApplication::instance())
        return;

    if (!w || !w->thread->isRunning())
        return;

    QThread *current = QThread::currentThread();
    if (current != QCoreApplication::instance()->thread() && (current != w->thread || !m_lockedForSync)) {
        qWarning() << "Updates can only be scheduled from GUI thread or from QQuickItem::updatePaintNode()";
        return;
    }

    if (!w || !w->thread->isRunning()) {
        return;
    }
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "update from item" << w->window;

    // Call this function from the Gui thread later as startTimer cannot be
    // called from the render thread.
    if (current == w->thread) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- on render thread";
        w->updateDuringSync = true;
        return;
    }

    maybePostPolishRequest(w);
}

/*!
    Called when the QQuickWindow should be explicitly repainted. This function
    can also be called on the render thread when the GUI thread is blocked to
    keep render thread animations alive.
 */
void ThreadedRenderLoop::update(QQuickWindow *window)
{
    Window *w = windowFor(m_windows, window);
    if (!w)
        return;

    if (w->thread == QThread::currentThread()) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "update on window - on render thread" << w->window;
        w->thread->requestRepaint();
        return;
    }

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "update on window" << w->window;
    // We set forceRenderPass because we want to make sure the QQuickWindow
    // actually does a full render pass after the next sync.
    w->forceRenderPass = true;
    maybeUpdate(w);
}


void ThreadedRenderLoop::releaseResources(QQuickWindow *window)
{
    Window *w = windowFor(m_windows, window);
    if (w)
        releaseResources(w, false);
}

/*!
 * Release resources will post an event to the render thread to
 * free up the SG and GL resources and exists the render thread.
 */
void ThreadedRenderLoop::releaseResources(Window *w, bool inDestructor)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "releaseResources()" << (inDestructor ? "in destructor" : "in api-call") << w->window;

    w->thread->mutex.lock();
    if (w->thread->isRunning() && w->thread->active) {
        QQuickWindow *window = w->window;

        // The platform window might have been destroyed before
        // hide/release/windowDestroyed is called, so we need to have a
        // fallback surface to perform the cleanup of the scene graph
        // and the OpenGL resources.
        // QOffscreenSurface must be created on the GUI thread, so we
        // create it here and pass it on to RenderThread::invalidateGL()
        QOffscreenSurface *fallback = 0;
        if (!window->handle()) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- using fallback surface";
            fallback = new QOffscreenSurface();
            fallback->setFormat(w->actualWindowFormat);
            fallback->create();
        }

        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- posting release request to render thread";
        w->thread->postEvent(new WMTryReleaseEvent(window, inDestructor, fallback));
        w->thread->waitCondition.wait(&w->thread->mutex);
        delete fallback;

        // Avoid a shutdown race condition.
        // If SG is invalidated and 'active' becomes false, the thread's run()
        // method will exit. handleExposure() relies on QThread::isRunning() (because it
        // potentially needs to start the thread again) and our mutex cannot be used to
        // track the thread stopping, so we wait a few nanoseconds extra so the thread
        // can exit properly.
        if (!w->thread->active) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << " - waiting for render thread to exit" << w->window;
            w->thread->wait();
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << " - render thread finished" << w->window;
        }
    }
    w->thread->mutex.unlock();
}


/* Calls polish on all items, then requests synchronization with the render thread
 * and blocks until that is complete. Returns false if it aborted; otherwise true.
 */
void ThreadedRenderLoop::polishAndSync(Window *w, bool inExpose)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "polishAndSync" << (inExpose ? "(in expose)" : "(normal)") << w->window;

    QQuickWindow *window = w->window;
    if (!w->thread || !w->thread->window) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- not exposed, abort";
        return;
    }

    // Flush pending touch events.
    QQuickWindowPrivate::get(window)->flushDelayedTouchEvent();
    // The delivery of the event might have caused the window to stop rendering
    w = windowFor(m_windows, window);
    if (!w || !w->thread || !w->thread->window) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- removed after event flushing, abort";
        return;
    }


    QElapsedTimer timer;
    qint64 polishTime = 0;
    qint64 waitTime = 0;
    qint64 syncTime = 0;
    bool profileFrames = QSG_RASTER_LOG_TIME_RENDERLOOP().isDebugEnabled();
    if (profileFrames)
        timer.start();
    Q_QUICK_SG_PROFILE_START(QQuickProfiler::SceneGraphPolishAndSync);

    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    d->polishItems();

    if (profileFrames)
        polishTime = timer.nsecsElapsed();
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync);

    w->updateDuringSync = false;

    emit window->afterAnimating();

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- lock for sync";
    w->thread->mutex.lock();
    m_lockedForSync = true;
    w->thread->postEvent(new WMSyncEvent(window, inExpose, w->forceRenderPass));
    w->forceRenderPass = false;

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- wait for sync";
    if (profileFrames)
        waitTime = timer.nsecsElapsed();
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync);
    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- unlock after sync";

    if (profileFrames)
        syncTime = timer.nsecsElapsed();
    Q_QUICK_SG_PROFILE_RECORD(QQuickProfiler::SceneGraphPolishAndSync);

    if (m_animation_timer == 0 && m_animation_driver->isRunning()) {
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- advancing animations";
        m_animation_driver->advance();
        qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- animations done..";
        // We need to trigger another sync to keep animations running...
        maybePostPolishRequest(w);
        emit timeToIncubate();
    } else if (w->updateDuringSync) {
        maybePostPolishRequest(w);
    }

    qCDebug(QSG_RASTER_LOG_TIME_RENDERLOOP()).nospace()
            << "Frame prepared with 'threaded' renderloop"
            << ", polish=" << (polishTime / 1000000)
            << ", lock=" << (waitTime - polishTime) / 1000000
            << ", blockedForSync=" << (syncTime - waitTime) / 1000000
            << ", animations=" << (timer.nsecsElapsed() - syncTime) / 1000000
            << " - (on Gui thread) " << window;

    Q_QUICK_SG_PROFILE_END(QQuickProfiler::SceneGraphPolishAndSync);
}

bool ThreadedRenderLoop::event(QEvent *e)
{
    switch ((int) e->type()) {

    case QEvent::Timer: {
        QTimerEvent *te = static_cast<QTimerEvent *>(e);
        if (te->timerId() == m_animation_timer) {
            qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- ticking non-visual timer";
            m_animation_driver->advance();
            emit timeToIncubate();
            return true;
        }
    }

    default:
        break;
    }

    return QObject::event(e);
}



/*
    Locks down GUI and performs a grab the scene graph, then returns the result.

    Since the QML scene could have changed since the last time it was rendered,
    we need to polish and sync the scene graph. This might seem superfluous, but
     - QML changes could have triggered deleteLater() which could have removed
       textures or other objects from the scene graph, causing render to crash.
     - Autotests rely on grab(), setProperty(), grab(), compare behavior.
 */

QImage ThreadedRenderLoop::grab(QQuickWindow *window)
{
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "grab()" << window;

    Window *w = windowFor(m_windows, window);
    Q_ASSERT(w);

    if (!w->thread->isRunning())
        return QImage();

    if (!window->handle())
        window->create();

    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- polishing items";
    QQuickWindowPrivate *d = QQuickWindowPrivate::get(window);
    d->polishItems();

    QImage result;
    w->thread->mutex.lock();
    m_lockedForSync = true;
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- posting grab event";
    w->thread->postEvent(new WMGrabEvent(window, &result));
    w->thread->waitCondition.wait(&w->thread->mutex);
    m_lockedForSync = false;
    w->thread->mutex.unlock();
    qCDebug(QSG_RASTER_LOG_RENDERLOOP) << "- grab complete";

    return result;
}

/*!
 * Posts a new job event to the render thread.
 * Returns true if posting succeeded.
 */
void ThreadedRenderLoop::postJob(QQuickWindow *window, QRunnable *job)
{
    Window *w = windowFor(m_windows, window);
    if (w && w->thread && w->thread->window)
        w->thread->postEvent(new WMJobEvent(window, job));
    else
        delete job;
}

#include "threadedrenderloop.moc"

QT_END_NAMESPACE
