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
  _pipeWireNodeId(0),
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
  QTimer::singleShot(30000, _eventLoop, &QEventLoop::quit);
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

  // Set flag so the Response handler knows to call OpenPipeWireRemote
  _waitingForStart = true;

  // The Response signal will arrive asynchronously and the handler will:
  // 1. Call OpenPipeWireRemote
  // 2. Set _pipeWireFd
  // 3. Quit any waiting event loop

  // For now, just return true - the FD will be set asynchronously
  qDebug() << "Start initiated - FD will be set when Response arrives";
  return true;
}

bool VideoScreenPipeWireImpl::connectToResponseSignal(const QString& requestPath)
{
  // Connect to the Response signal with proper signature: (uint response, QVariantMap results)
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

void VideoScreenPipeWireImpl::onPortalResponse(uint response, const QVariantMap& results)
{
  qDebug() << "Portal Response received - response code:" << response;
  qDebug() << "  _waitingForStart:" << _waitingForStart;
  qDebug() << "  _sessionHandle:" << _sessionHandle;
  qDebug() << "  Results keys:" << results.keys();

  // Response code 0 = success, 1 = cancelled, 2 = other error
  if (response != 0) {
    qWarning() << "Portal request cancelled or failed with code:" << response;
    if (_eventLoop && _eventLoop->isRunning()) {
      _eventLoop->quit();
    }
    return;
  }

  // Mark session as ready - for SelectSources this is all we need
  _sessionReady = true;

  // If this is the Start response, extract the PipeWire node ID from streams
  if (_waitingForStart && results.contains("streams")) {
    qDebug() << "This is the Start response - parsing streams for node ID";

    QVariant streamsVariant = results["streams"];
    qDebug() << "Streams variant type:" << streamsVariant.typeName();

    // The streams field is a D-Bus array of structs: a(ua{sv})
    // Format: [(node_id: uint, properties: dict), ...]
    // We need to parse it as a QDBusArgument

    if (streamsVariant.canConvert<QDBusArgument>()) {
      qDebug() << "Parsing streams as QDBusArgument (D-Bus a(ua{sv}) format)";

      const QDBusArgument argument = streamsVariant.value<QDBusArgument>();

      // Begin array iteration
      argument.beginArray();

      while (!argument.atEnd()) {
        // Begin struct iteration - each stream is (node_id, properties)
        argument.beginStructure();

        // First element: node_id (uint)
        uint nodeId;
        argument >> nodeId;

        // Second element: properties (a{sv} - dict of string to variant)
        QVariantMap properties;
        argument >> properties;

        argument.endStructure();

        qDebug() << "Found stream - node_id:" << nodeId << "properties:" << properties.keys();

        // Use the first stream's node ID
        if (_pipeWireNodeId == 0) {
          _pipeWireNodeId = nodeId;
          qDebug() << "Successfully extracted PipeWire node ID:" << _pipeWireNodeId;
        }
      }

      argument.endArray();

      if (_pipeWireNodeId == 0) {
        qWarning() << "No streams found in D-Bus array";
      }

    } else {
      qWarning() << "Streams is not a QDBusArgument - unexpected type:" << streamsVariant.typeName();
    }
  }

  qDebug() << "Checking event loop: _eventLoop =" << (void*)_eventLoop
           << "isRunning =" << (_eventLoop ? _eventLoop->isRunning() : false);

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
    qWarning() << "FAILED: Could not initiate stream start";
    return false;
  }
  qDebug() << "SUCCESS: Stream start initiated";

  // Wait for Start Response
  _eventLoop = new QEventLoop();
  QTimer::singleShot(10000, _eventLoop, &QEventLoop::quit);

  qDebug() << "Waiting for Start Response, event loop =" << (void*)_eventLoop;

  _eventLoop->exec();

  delete _eventLoop;
  _eventLoop = NULL;

  qDebug() << "Event loop exited, _pipeWireNodeId =" << _pipeWireNodeId;

  if (_pipeWireNodeId == 0) {
    qWarning() << "FAILED: Did not get PipeWire node ID";
    return false;
  }

  qDebug() << "========================================";
  qDebug() << "SUCCESS: Got PipeWire node ID" << _pipeWireNodeId;
  qDebug() << "========================================";
  return true;
}

bool VideoScreenPipeWireImpl::loadMovie(const QString& path)
{
  // If _pipeWireNodeId is not set, try to parse it from the path
  if (_pipeWireNodeId == 0) {
    bool ok;
    uint nodeId = path.toUInt(&ok);
    if (ok && nodeId > 0) {
      _pipeWireNodeId = nodeId;
      qDebug() << "Loaded PipeWire node ID from path:" << _pipeWireNodeId;
    } else {
      qWarning() << "No valid PipeWire node ID available";
      return false;
    }
  }

  qDebug() << "Setting up PipeWire screen capture pipeline with node:" << _pipeWireNodeId;

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

  // Create videoflip element to fix mirrored output
  GstElement *videoflip = gst_element_factory_make("videoflip", "videoflip0");
  if (!videoflip)
  {
    qWarning() << "videoflip element not available.";
    unloadMovie();
    return false;
  }

  // Set flip method to horizontal flip (method=4)
  g_object_set(videoflip, "method", 4, NULL);

  // Add to pipeline
  gst_bin_add_many(GST_BIN(_pipeline), _pipewiresrc0, videoflip, NULL);

  // Link pipewiresrc -> videoflip -> queue
  if (!gst_element_link_many(_pipewiresrc0, videoflip, _queue0, NULL))
  {
    qWarning() << "Could not link pipewiresrc to videoflip to queue.";
    unloadMovie();
    return false;
  }

  // Configure PipeWire source with the node ID from portal
  // The pipewiresrc element expects the "path" property to be set to the node ID as a string
  QString nodePath = QString::number(_pipeWireNodeId);
  g_object_set(_pipewiresrc0,
               "path", nodePath.toUtf8().constData(),
               "client-name", "MapMap",
               NULL);

  qDebug() << "Configured pipewiresrc with node path:" << nodePath;

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
