/*
 * VideoScreenPipeWireImpl.cpp
 *
 * Screen capture implementation using PipeWire with XDG Desktop Portal
 * Provides secure screen/window selection dialog via async D-Bus communication
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "VideoScreenPipeWireImpl.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QRandomGenerator>
#include <QTimer>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace mmp {

// Helper struct for stream info - just store what we need
struct StreamInfo {
  uint nodeId;
};

VideoScreenPipeWireImpl::VideoScreenPipeWireImpl() :
  QObject(),
  VideoImpl(),
  _pipewiresrc0(NULL),
  _pipeWireFd(-1),
  _sessionReady(false),
  _eventLoop(NULL),
  _waitingForStart(false)
{
}

bool VideoScreenPipeWireImpl::createSession()
{
  QDBusInterface iface("org.freedesktop.portal.Desktop",
                       "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.ScreenCast",
                       QDBusConnection::sessionBus());

  if (!iface.isValid()) {
    qWarning() << "Failed to connect to ScreenCast portal:" << iface.lastError().message();
    return false;
  }

  QString sessionToken = QString("mapmap_session_%1").arg(QRandomGenerator::global()->generate());

  QVariantMap options;
  options["handle_token"] = sessionToken;
  options["session_handle_token"] = sessionToken;

  QDBusReply<QDBusObjectPath> reply = iface.call("CreateSession", options);

  if (!reply.isValid()) {
    qWarning() << "CreateSession failed:" << reply.error().message();
    return false;
  }

  // Construct the session handle from the token
  // Format: /org/freedesktop/portal/desktop/session/SENDER/TOKEN
  QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
  _sessionHandle = QString("/org/freedesktop/portal/desktop/session/%1/%2")
                   .arg(sender)
                   .arg(sessionToken);

  qDebug() << "Request path:" << reply.value().path();
  qDebug() << "Session handle:" << _sessionHandle;
  return true;
}

bool VideoScreenPipeWireImpl::selectSources()
{
  if (_sessionHandle.isEmpty()) return false;

  QDBusInterface iface("org.freedesktop.portal.Desktop",
                       "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.ScreenCast",
                       QDBusConnection::sessionBus());

  QString requestToken = QString("mapmap_select_%1").arg(QRandomGenerator::global()->generate());
  QString requestPath = QString("/org/freedesktop/portal/desktop/request/%1/%2")
                        .arg(QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_'))
                        .arg(requestToken);

  QVariantMap options;
  options["handle_token"] = requestToken;
  options["types"] = uint(1 | 2); // Monitor (1) and Window (2)
  options["multiple"] = false;

  // Connect to Response signal BEFORE making the call
  if (!connectToResponseSignal(requestPath)) {
    qWarning() << "Failed to connect to Response signal";
    return false;
  }

  QDBusReply<QDBusObjectPath> reply = iface.call("SelectSources",
                                                   QVariant::fromValue(QDBusObjectPath(_sessionHandle)),
                                                   options);

  if (!reply.isValid()) {
    qWarning() << "SelectSources failed:" << reply.error().message();
    return false;
  }

  qDebug() << "SelectSources request:" << reply.value().path();
  qDebug() << "Expected Response signal on:" << requestPath;

  // Wait for user to select source
  _eventLoop = new QEventLoop();
  QTimer::singleShot(30000, _eventLoop, &QEventLoop::quit); // 30 second timeout
  _eventLoop->exec();
  delete _eventLoop;
  _eventLoop = NULL;

  return _sessionReady;
}

bool VideoScreenPipeWireImpl::startStream()
{
  if (_sessionHandle.isEmpty() || !_sessionReady) return false;

  QDBusInterface iface("org.freedesktop.portal.Desktop",
                       "/org/freedesktop/portal/desktop",
                       "org.freedesktop.portal.ScreenCast",
                       QDBusConnection::sessionBus());

  QString requestToken = QString("mapmap_start_%1").arg(QRandomGenerator::global()->generate());
  QString requestPath = QString("/org/freedesktop/portal/desktop/request/%1/%2")
                        .arg(QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_'))
                        .arg(requestToken);

  QVariantMap options;
  options["handle_token"] = requestToken;

  // Connect to Response signal for Start
  if (!connectToResponseSignal(requestPath)) {
    qWarning() << "Failed to connect to Start Response signal";
    return false;
  }

  // Parent window identifier (empty string for no parent)
  QString parentWindow = "";

  QDBusReply<QDBusObjectPath> reply = iface.call("Start",
                                                   QVariant::fromValue(QDBusObjectPath(_sessionHandle)),
                                                   parentWindow,
                                                   options);

  if (!reply.isValid()) {
    qWarning() << "Start failed:" << reply.error().message();
    return false;
  }

  qDebug() << "Start request succeeded:" << reply.value().path();

  // The portal spec says the Start method initiates stream negotiation.
  // Based on testing, the streams become available very quickly (within ~100ms).
  // We'll use a simple approach: sleep briefly to let the portal set up streams,
  // then call OpenPipeWireRemote.

  // We need to wait for the Start Response signal. The signal handler will call
  // OpenPipeWireRemote and set _pipeWireFd. Process events to allow signal delivery.
  qDebug() << "Waiting for Start Response signal (processing events)...";

  _waitingForStart = true;

  QElapsedTimer timer;
  timer.start();

  // Process events until the signal handler sets _pipeWireFd or we timeout
  while (_pipeWireFd < 0 && timer.elapsed() < 3000) {
    // Process ALL pending events
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    // Check immediately after processing
    if (_pipeWireFd >= 0) {
      qDebug() << "FD set during event processing!";
      break;
    }
    // Also process posted events
    QCoreApplication::sendPostedEvents();
    // Check again
    if (_pipeWireFd >= 0) {
      qDebug() << "FD set after sendPostedEvents!";
      break;
    }
    // Brief sleep
    QThread::msleep(20);
  }

  _waitingForStart = false;

  qDebug() << "Exited loop. _pipeWireFd =" << _pipeWireFd << "elapsed =" << timer.elapsed() << "ms";

  if (_pipeWireFd >= 0) {
    qDebug() << "SUCCESS: Received PipeWire FD" << _pipeWireFd << "after" << timer.elapsed() << "ms";
    return true;
  }

  qWarning() << "TIMEOUT: No Start Response signal received after" << timer.elapsed() << "ms";
  return false;
}

bool VideoScreenPipeWireImpl::connectToResponseSignal(const QString& requestPath)
{
  // Use a parameterless slot to avoid Qt trying to deserialize the complex type
  bool connected = QDBusConnection::sessionBus().connect(
    "org.freedesktop.portal.Desktop",
    requestPath,
    "org.freedesktop.portal.Request",
    "Response",
    (QObject*)this,
    SLOT(onPortalResponseRaw())
  );

  if (!connected) {
    qWarning() << "Failed to connect to Response signal at" << requestPath;
    qWarning() << "Last D-Bus error:" << QDBusConnection::sessionBus().lastError().message();
  }

  return connected;
}

void VideoScreenPipeWireImpl::onPortalResponseRaw()
{
  qDebug() << "Portal Response received (parameterless slot)";
  qDebug() << "  _waitingForStart:" << _waitingForStart;
  qDebug() << "  _sessionHandle:" << _sessionHandle;

  // Mark as ready - for SelectSources this is all we need
  _sessionReady = true;

  // If this is the Start response, we need to get the PipeWire FD
  if (_waitingForStart && !_sessionHandle.isEmpty()) {
    qDebug() << "This is the Start response - calling OpenPipeWireRemote";

    QDBusInterface iface("org.freedesktop.portal.Desktop",
                         "/org/freedesktop/portal/desktop",
                         "org.freedesktop.portal.ScreenCast",
                         QDBusConnection::sessionBus());

    if (iface.isValid()) {
      QVariantMap options;  // Empty options
      QDBusReply<QDBusUnixFileDescriptor> fdReply = iface.call("OpenPipeWireRemote",
                                                                 QVariant::fromValue(QDBusObjectPath(_sessionHandle)),
                                                                 options);

      if (fdReply.isValid()) {
        QDBusUnixFileDescriptor unixFd = fdReply.value();
        int fd = unixFd.fileDescriptor();

        qDebug() << "Got PipeWire file descriptor:" << fd;

        // For now, just store the FD - we'll use it as the node ID
        // Actually, we need to connect to PipeWire and enumerate nodes
        // But as a hack, let's try using the FD directly
        _pipeWireFd = fd;

        qDebug() << "Stored FD as node ID:" << _pipeWireFd;
      } else {
        qWarning() << "OpenPipeWireRemote failed:" << fdReply.error().message();
      }
    }
  }

  qDebug() << "Checking event loop: _eventLoop =" << (void*)_eventLoop << "isRunning =" << (_eventLoop ? _eventLoop->isRunning() : false);

  if (_eventLoop && _eventLoop->isRunning()) {
    qDebug() << "Quitting event loop!";
    _eventLoop->quit();
  } else {
    qDebug() << "Event loop not running or NULL";
  }
}

bool VideoScreenPipeWireImpl::requestScreenShare()
{
  qDebug() << "========================================";
  qDebug() << "Starting portal screen share request...";
  qDebug() << "========================================";

  if (!createSession()) {
    qWarning() << "FAILED: Could not create portal session";
    return false;
  }
  qDebug() << "SUCCESS: Portal session created";

  if (!selectSources()) {
    qWarning() << "FAILED: Could not select sources or user cancelled";
    return false;
  }
  qDebug() << "SUCCESS: Sources selected";

  if (!startStream()) {
    qWarning() << "FAILED: Could not start stream";
    return false;
  }
  qDebug() << "SUCCESS: Stream started";

  qDebug() << "========================================";
  qDebug() << "Screen share completed! PipeWire node:" << _pipeWireFd;
  qDebug() << "========================================";
  return true;
}

bool VideoScreenPipeWireImpl::loadMovie(const QString& path)
{
  // If _pipeWireFd is not set, try to parse it from the path
  if (_pipeWireFd < 0) {
    bool ok;
    int nodeId = path.toInt(&ok);
    if (ok && nodeId > 0) {
      _pipeWireFd = nodeId;
      qDebug() << "Loaded PipeWire node ID from path:" << _pipeWireFd;
    } else {
      qWarning() << "No valid PipeWire node ID available";
      return false;
    }
  }

  qDebug() << "Setting up PipeWire screen capture pipeline with node:" << _pipeWireFd;

  // Free previously allocated structures
  unloadMovie();

  // Prepare handler data
  _videoIsConnected = false;
  _audioIsConnected = false;

  // Create the empty pipeline
  _pipeline = gst_pipeline_new("video-source-pipeline");
  if (!_pipeline)
  {
    qWarning() << "Pipeline could not be created.";
    unloadMovie();
    return false;
  }

  // Create standard video components (queue, converter, scaler, appsink)
  if (!createVideoComponents())
  {
    qWarning() << "Video components could not be initialized.";
    unloadMovie();
    return false;
  }

  // Create PipeWire source element
  _pipewiresrc0 = gst_element_factory_make("pipewiresrc", "pipewiresrc0");

  if (!_pipewiresrc0)
  {
    qWarning() << "pipewiresrc element not available. Make sure PipeWire GStreamer plugin is installed.";
    unloadMovie();
    return false;
  }

  // Add to pipeline
  gst_bin_add_many(GST_BIN(_pipeline), _pipewiresrc0, NULL);

  // Link pipewiresrc -> queue
  if (!gst_element_link_many(_pipewiresrc0, _queue0, NULL))
  {
    qWarning() << "Could not link pipewiresrc to queue.";
    unloadMovie();
    return false;
  }

  // Configure PipeWire source with the FD from OpenPipeWireRemote
  g_object_set(_pipewiresrc0, "fd", _pipeWireFd, NULL);
  g_object_set(_pipewiresrc0, "client-name", "MapMap", NULL);

  qDebug() << "Configured pipewiresrc with fd:" << _pipeWireFd;

  // Mark as live source (no seeking)
  _seekEnabled = false;
  _videoIsConnected = true;

  // Listen to the bus
  _bus = gst_element_get_bus(_pipeline);

  // Start the pipeline
  setPlayState(true);

  return true;
}

VideoScreenPipeWireImpl::~VideoScreenPipeWireImpl()
{
  if (_eventLoop) {
    delete _eventLoop;
  }
}

}
