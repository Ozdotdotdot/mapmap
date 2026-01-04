/*
 * VideoScreenPipeWireImpl.h
 *
 * Screen capture implementation using PipeWire (modern Wayland/X11 approach)
 * Uses XDG Desktop Portal for secure screen/window selection
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

#ifndef VIDEO_SCREEN_PIPEWIRE_IMPL_H_
#define VIDEO_SCREEN_PIPEWIRE_IMPL_H_

#include "VideoImpl.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QObject>

namespace mmp {

class VideoScreenPipeWireImpl : public QObject, public VideoImpl
{
  Q_OBJECT

public:
  VideoScreenPipeWireImpl();
  ~VideoScreenPipeWireImpl();
  bool loadMovie(const QString& path);
  bool isLive() { return true; }

  // Trigger portal screen selection (async)
  bool requestScreenShare();

  // Check if portal session is ready
  bool isSessionReady() const { return _sessionReady; }
  uint getPipeWireNodeId() const { return _pipeWireNodeId; }

private slots:
  void onPortalResponse(uint response, const QVariantMap& results);

private:
  GstElement *_pipewiresrc0;
  QString _sessionHandle;
  uint _pipeWireNodeId;  // PipeWire node ID from portal streams
  bool _sessionReady;
  QEventLoop *_eventLoop;

  // Portal helpers
  bool createSession();
  bool selectSources();
  bool startStream();

  // D-Bus signal connection
  enum class PortalRequestType {
    None,
    CreateSession,
    SelectSources,
    StartStream
  };
  bool connectToResponseSignal(const QString& requestPath, PortalRequestType type);
  PortalRequestType _pendingRequestType;

  // State tracking for async flow
  enum class PortalState {
    Idle,
    SessionCreated,
    SourcesSelected,
    StreamStarted
  };
  PortalState _portalState;
};

}

#endif
