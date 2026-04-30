(() => {
  let janus = null;
  let managerHandle = null;
  let subscriberHandle = null;

  const statusEl = document.getElementById('status');
  const videoEl = document.getElementById('remoteVideo');
  const connectBtn = document.getElementById('connectBtn');
  const disconnectBtn = document.getElementById('disconnectBtn');

  const setStatus = (msg) => { statusEl.textContent = msg; };

  const janusServer = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/janus-ws`;

  async function destroySession() {
    if (janus) {
      await new Promise((resolve) => janus.destroy({ success: resolve, error: resolve }));
    }
    janus = null;
    managerHandle = null;
    subscriberHandle = null;
    videoEl.srcObject = null;
    connectBtn.disabled = false;
    disconnectBtn.disabled = true;
  }

  function attachSubscriber(feedId, roomId, roomPin) {
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
      error: (err) => setStatus(`Attach subscriber failed: ${err}`),
      onmessage: (msg, jsep) => {
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
      onremotetrack: (_track, _mid, added, metadata) => {
        if (!added) {
          return;
        }
        const stream = metadata && metadata.stream;
        if (stream) {
          Janus.attachMediaStream(videoEl, stream);
          setStatus('Streaming from ESP32');
        }
      },
      oncleanup: () => {
        videoEl.srcObject = null;
      },
    });
  }

  function attachManager(roomId, roomPin) {
    janus.attach({
      plugin: 'janus.plugin.videoroom',
      success: (pluginHandle) => {
        managerHandle = pluginHandle;
        const req = { request: 'listparticipants', room: roomId };
        if (roomPin) {
          req.pin = roomPin;
        }
        managerHandle.send({
          message: req,
          success: (resp) => {
            const participants = resp && resp.participants ? resp.participants : [];
            if (!participants.length) {
              setStatus('No active publisher found in room. Ensure ESP32 is connected.');
              return;
            }
            const feedId = participants[0].id;
            attachSubscriber(feedId, roomId, roomPin);
          },
          error: (err) => setStatus(`listparticipants failed: ${err}`),
        });
      },
      error: (err) => setStatus(`Attach manager failed: ${err}`),
    });
  }

  async function connect() {
    connectBtn.disabled = true;
    disconnectBtn.disabled = false;

    const roomId = parseInt(document.getElementById('roomId').value, 10);
    const roomPin = document.getElementById('roomPin').value.trim();
    const token = document.getElementById('token').value.trim();

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
