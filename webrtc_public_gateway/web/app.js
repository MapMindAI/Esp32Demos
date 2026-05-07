(() => {
  let janus = null;
  let managerHandle = null;
  let subscriberHandle = null;
  let selectedFeedId = null;
  let remoteStream = null;
  let statsTimer = null;
  let prevVideoBytes = 0;
  let prevStatsTs = 0;

  const statusEl = document.getElementById('status');
  const videoEl = document.getElementById('remoteVideo');
  const connectBtn = document.getElementById('connectBtn');
  const disconnectBtn = document.getElementById('disconnectBtn');

  const setStatus = (msg) => { statusEl.textContent = msg; };

  const janusServer = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/janus-ws`;

  async function destroySession() {
    if (statsTimer) {
      clearInterval(statsTimer);
      statsTimer = null;
    }
    if (janus) {
      await new Promise((resolve) => janus.destroy({ success: resolve, error: resolve }));
    }
    janus = null;
    managerHandle = null;
    subscriberHandle = null;
    selectedFeedId = null;
    remoteStream = null;
    prevVideoBytes = 0;
    prevStatsTs = 0;
    videoEl.srcObject = null;
    connectBtn.disabled = false;
    disconnectBtn.disabled = true;
  }

  function attachSubscriber(feedId, roomId, roomPin) {
    selectedFeedId = feedId;
    remoteStream = new MediaStream();
    videoEl.srcObject = remoteStream;
    setStatus(`Subscribing to feed ${feedId}...`);
    janus.attach({
      plugin: 'janus.plugin.videoroom',
      success: (pluginHandle) => {
        subscriberHandle = pluginHandle;
        const join = {
          request: 'join',
          ptype: 'subscriber',
          room: roomId,
          streams: [{ feed: feedId }],
        };
        if (roomPin) {
          join.pin = roomPin;
        }
        subscriberHandle.send({ message: join });
      },
      error: (err) => {
        selectedFeedId = null;
        setStatus(`Attach subscriber failed: ${err}`);
      },
      onmessage: (msg, jsep) => {
        if (msg?.videoroom === 'event' && msg?.error) {
          const errText = String(msg.error);
          if (errText.includes('No such feed')) {
            setStatus('Feed disappeared, refreshing publisher list...');
            if (subscriberHandle) {
              subscriberHandle.detach();
              subscriberHandle = null;
            }
            selectedFeedId = null;
            requestAndAttachPublisher(roomId, roomPin);
            return;
          }
        }
        if (msg.videoroom === 'attached') {
          setStatus('Attached to publisher feed.');
        }
        if (jsep) {
          subscriberHandle.createAnswer({
            jsep,
            tracks: [{ type: 'audio', capture: false, recv: true }, { type: 'video', capture: false, recv: true }],
            success: (jsepAnswer) => {
              subscriberHandle.send({ message: { request: 'start', room: roomId }, jsep: jsepAnswer });
            },
            error: (err) => setStatus(`Answer failed: ${err}`),
          });
        }
      },
      onremotetrack: (track, _mid, added, _metadata) => {
        if (!remoteStream) {
          remoteStream = new MediaStream();
          videoEl.srcObject = remoteStream;
        }
        if (!added) {
          remoteStream.getTracks().forEach((t) => {
            if (t.id === track.id) {
              remoteStream.removeTrack(t);
            }
          });
          return;
        }
        if (!remoteStream.getTracks().some((t) => t.id === track.id)) {
          remoteStream.addTrack(track);
        }
        videoEl.play().catch(() => {});
        setStatus('Streaming from ESP32');
        startConnectionQualityLogs();
      },
      webrtcState: (on) => {
        console.log(`[webrtc] state=${on ? 'up' : 'down'}`);
      },
      iceState: (state) => {
        console.log(`[webrtc] iceState=${state}`);
      },
      oncleanup: () => {
        if (statsTimer) {
          clearInterval(statsTimer);
          statsTimer = null;
        }
        videoEl.srcObject = null;
        remoteStream = null;
        prevVideoBytes = 0;
        prevStatsTs = 0;
        subscriberHandle = null;
      },
    });
  }

  async function sampleConnectionQuality() {
    try {
      if (!subscriberHandle?.webrtcStuff?.pc) {
        return;
      }
      const pc = subscriberHandle.webrtcStuff.pc;
      const stats = await pc.getStats();

      let selectedPair = null;
      let remoteInboundVideo = null;
      let inboundVideo = null;

      stats.forEach((r) => {
        if (r.type === 'transport' && r.selectedCandidatePairId && stats.get(r.selectedCandidatePairId)) {
          selectedPair = stats.get(r.selectedCandidatePairId);
        }
        if (!selectedPair && r.type === 'candidate-pair' && r.selected) {
          selectedPair = r;
        }
        if (r.type === 'inbound-rtp' && r.kind === 'video' && !inboundVideo) {
          inboundVideo = r;
        }
        if (r.type === 'remote-inbound-rtp' && r.kind === 'video' && !remoteInboundVideo) {
          remoteInboundVideo = r;
        }
      });

      let bitrateKbps = 0;
      if (inboundVideo && typeof inboundVideo.bytesReceived === 'number' && typeof inboundVideo.timestamp === 'number') {
        if (prevStatsTs > 0 && inboundVideo.timestamp > prevStatsTs) {
          const dtSec = (inboundVideo.timestamp - prevStatsTs) / 1000;
          const dBytes = inboundVideo.bytesReceived - prevVideoBytes;
          bitrateKbps = dtSec > 0 ? (dBytes * 8) / dtSec / 1000 : 0;
        }
        prevVideoBytes = inboundVideo.bytesReceived;
        prevStatsTs = inboundVideo.timestamp;
      }

      const rttMs = selectedPair?.currentRoundTripTime ? Math.round(selectedPair.currentRoundTripTime * 1000) : null;
      const jitterMs = remoteInboundVideo?.jitter ? Math.round(remoteInboundVideo.jitter * 1000) : null;
      const fps = inboundVideo?.framesPerSecond ?? null;
      const dropped = inboundVideo?.framesDropped ?? null;
      const decoded = inboundVideo?.framesDecoded ?? null;

      const emoji = qualityEmoji(pc.iceConnectionState, rttMs, jitterMs, bitrateKbps);
      const quality = `${emoji} ICE:${pc.iceConnectionState} RTT:${rttMs ?? '-'}ms Bitrate:${Math.round(bitrateKbps)}kbps FPS:${fps ?? '-'} Jitter:${jitterMs ?? '-'}ms Drop:${dropped ?? '-'} Dec:${decoded ?? '-'}`;
      console.log(`[quality] ${quality}`);
      setStatus(`Streaming from ESP32 | ${quality}`);
    } catch (err) {
      console.log(`[quality] stats error: ${err}`);
    }
  }

  function startConnectionQualityLogs() {
    if (statsTimer) {
      return;
    }
    statsTimer = setInterval(sampleConnectionQuality, 2000);
  }

  function qualityEmoji(iceState, rttMs, jitterMs, bitrateKbps) {
    if (iceState !== 'connected' && iceState !== 'completed') {
      return '🔴';
    }
    if ((rttMs !== null && rttMs > 300) || (jitterMs !== null && jitterMs > 80) || bitrateKbps < 250) {
      return '🟠';
    }
    if ((rttMs !== null && rttMs > 150) || (jitterMs !== null && jitterMs > 40) || bitrateKbps < 600) {
      return '🟡';
    }
    return '🟢';
  }

  function requestAndAttachPublisher(roomId, roomPin) {
    if (!managerHandle) {
      return;
    }
    const req = { request: 'listparticipants', room: roomId };
    if (roomPin) {
      req.pin = roomPin;
    }
    managerHandle.send({
      message: req,
      success: (resp) => {
        const participants = resp?.participants || [];
        const publishers = participants.filter((p) => {
          if (!p || !p.id) {
            return false;
          }
          if (Array.isArray(p.streams) && p.streams.length > 0) {
            return true;
          }
          return p.publisher === true;
        });
        if (!publishers.length) {
          selectedFeedId = null;
          setStatus('No active publisher found in room. Ensure ESP32 is connected.');
          return;
        }
        const candidateFeedId = publishers[0].id;
        if (selectedFeedId === candidateFeedId && subscriberHandle) {
          setStatus(`Already subscribed to feed ${candidateFeedId}.`);
          return;
        }
        if (subscriberHandle) {
          subscriberHandle.detach();
          subscriberHandle = null;
        }
        attachSubscriber(candidateFeedId, roomId, roomPin);
      },
      error: (err) => setStatus(`listparticipants failed: ${err}`),
    });
  }

  function attachManager(roomId, roomPin) {
    janus.attach({
      plugin: 'janus.plugin.videoroom',
      success: (pluginHandle) => {
        managerHandle = pluginHandle;
        requestAndAttachPublisher(roomId, roomPin);
      },
      error: (err) => setStatus(`Attach manager failed: ${err}`),
      onmessage: (msg) => {
        if (msg?.videoroom === 'event' && (msg.publishers || msg.unpublished || msg.leaving)) {
          requestAndAttachPublisher(roomId, roomPin);
        }
      },
    });
  }

  async function connect() {
    if (!window.Janus) {
      setStatus('Janus JS library failed to load. Check Internet/CDN access, then refresh.');
      return;
    }

    connectBtn.disabled = true;
    disconnectBtn.disabled = false;

    const roomId = parseInt(document.getElementById('roomId').value, 10);
    const roomPin = document.getElementById('roomPin').value.trim();
    const token = document.getElementById('token').value.trim();
    const apiSecret = document.getElementById('apiSecret').value.trim();

    if (!Number.isFinite(roomId)) {
      setStatus('Invalid room ID.');
      connectBtn.disabled = false;
      disconnectBtn.disabled = true;
      return;
    }

    Janus.init({
      debug: 'all',
      callback: () => {
        janus = new Janus({
          server: janusServer,
          token: token || undefined,
          apisecret: apiSecret || undefined,
          success: () => {
            setStatus('Connected to Janus.');
            attachManager(roomId, roomPin);
          },
          error: (err) => {
            setStatus(`Janus session failed: ${err}`);
            connectBtn.disabled = false;
            disconnectBtn.disabled = true;
          },
          destroyed: async () => {
            await destroySession();
            setStatus('Disconnected.');
          },
        });
      },
    });
  }

  connectBtn.addEventListener('click', connect);
  disconnectBtn.addEventListener('click', async () => {
    await destroySession();
    setStatus('Disconnected.');
  });
})();
