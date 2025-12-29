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
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QRandomGenerator>
#include <QTimer>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace mmp {

VideoScreenPipeWireImpl::VideoScreenPipeWireImpl() :
  VideoImpl(),
  _pipewiresrc0(NULL),
  _pipeWireFd(-1),
  _sessionReady(false),
  _eventLoop(NULL)
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

  qDebug() << "Start request:" << reply.value().path();

  // Wait for Start response with PipeWire stream info
  _eventLoop = new QEventLoop();
  QTimer::singleShot(10000, _eventLoop, &QEventLoop::quit); // 10 second timeout
  _eventLoop->exec();
  delete _eventLoop;
  _eventLoop = NULL;

  return (_pipeWireFd >= 0);
}

bool VideoScreenPipeWireImpl::connectToResponseSignal(const QString& requestPath)
{
  // Note: this->metaObject() will work because we have Q_OBJECT
  bool connected = QDBusConnection::sessionBus().connect(
    "org.freedesktop.portal.Desktop",
    requestPath,
    "org.freedesktop.portal.Request",
    "Response",
    (QObject*)this,
    SLOT(onPortalResponse(uint, QVariantMap))
  );

  if (!connected) {
    qWarning() << "Failed to connect to Response signal at" << requestPath;
    qWarning() << "Last D-Bus error:" << QDBusConnection::sessionBus().lastError().message();
  }

  return connected;
}

void VideoScreenPipeWireImpl::onPortalResponse(uint response, const QVariantMap &results)
{
  qDebug() << "Portal Response received:" << response << results;

  if (response != 0) {
    qWarning() << "Portal request cancelled or failed. Response code:" << response;
    if (_eventLoop && _eventLoop->isRunning()) {
      _eventLoop->quit();
    }
    return;
  }

  // Check if this response contains stream information
  if (results.contains("streams")) {
    QDBusArgument streamsArg = results["streams"].value<QDBusArgument>();
    streamsArg.beginArray();

    while (!streamsArg.atEnd()) {
      streamsArg.beginStructure();
      uint nodeId;
      QVariantMap streamProperties;
      streamsArg >> nodeId >> streamProperties;
      streamsArg.endStructure();

      qDebug() << "PipeWire node ID:" << nodeId;
      qDebug() << "Stream properties:" << streamProperties;

      // Store the node ID for pipewiresrc
      _pipeWireFd = nodeId; // Actually this is the node ID, not FD
      _sessionReady = true;
      break; // Use first stream
    }

    streamsArg.endArray();
  } else {
    // This is the SelectSources response
    _sessionReady = true;
    qDebug() << "Source selection completed successfully";
  }

  if (_eventLoop && _eventLoop->isRunning()) {
    _eventLoop->quit();
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
  if (_pipeWireFd < 0) {
    qWarning() << "No PipeWire node available - screen share must be requested first";
    return false;
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

  // Configure PipeWire source with the node ID
  QString pipewirePath = QString("%1").arg(_pipeWireFd);
  g_object_set(_pipewiresrc0, "path", pipewirePath.toUtf8().constData(), NULL);
  g_object_set(_pipewiresrc0, "client-name", "MapMap", NULL);

  qDebug() << "Configured pipewiresrc with path:" << pipewirePath;

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
