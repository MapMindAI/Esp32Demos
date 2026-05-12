(() => {
  let janus = null;
  let managerHandle = null;
  let subscriberHandle = null;
  let selectedFeedId = null;
  let remoteStream = null;
  let statsTimer = null;
  let prevVideoBytes = 0;
  let prevStatsTs = 0;
  let lastVideoRxMs = 0;
  let videoStalled = false;
  let mqttClient = null;
  let heartbeatTimer = null;
  let pendingWebrtcOpen = false;
  let stallWindowStartMs = 0;
  let stallCountInWindow = 0;
  let lastRobotSpeedPublishMs = 0;
  let robotSpeedPublishTimer = null;
  let pendingRobotSpeedValue = null;
  let slamFrameTimer = null;
  let slamStateTimer = null;
  let slamRequestInflight = false;
  let slamRunning = false;
  let lastSlamStateFetchMs = 0;
  let slamLastSnapshot = null;

  const statusEl = document.getElementById('status');
  const videoEl = document.getElementById('remoteVideo');
  const connectBtn = document.getElementById('connectBtn');
  const disconnectBtn = document.getElementById('disconnectBtn');
  const roomIdEl = document.getElementById('roomId');
  const roomPinEl = document.getElementById('roomPin');
  const mqttWsUrlEl = document.getElementById('mqttWsUrl');
  const mqttUsernameEl = document.getElementById('mqttUsername');
  const mqttPasswordEl = document.getElementById('mqttPassword');
  const mqttMemTopicEl = document.getElementById('mqttMemTopic');
  const mqttCmdTopicEl = document.getElementById('mqttCmdTopic');
  const mqttConnectBtn = document.getElementById('mqttConnectBtn');
  const mqttDisconnectBtn = document.getElementById('mqttDisconnectBtn');
  const mqttCmdInputEl = document.getElementById('mqttCmdInput');
  const mqttSendBtn = document.getElementById('mqttSendBtn');
  const mqttStatusEl = document.getElementById('mqttStatus');
  const mqttLogEl = document.getElementById('mqttLog');
  const robotUpBtn = document.getElementById('robotUpBtn');
  const robotDownBtn = document.getElementById('robotDownBtn');
  const robotLeftBtn = document.getElementById('robotLeftBtn');
  const robotRightBtn = document.getElementById('robotRightBtn');
  const robotStopBtn = document.getElementById('robotStopBtn');
  const robotRotateLeftBtn = document.getElementById('robotRotateLeftBtn');
  const robotRotateRightBtn = document.getElementById('robotRotateRightBtn');
  const robotVelocitySlider = document.getElementById('robotVelocitySlider');
  const robotVelocityValue = document.getElementById('robotVelocityValue');
  const servoUpBtn = document.getElementById('servoUpBtn');
  const servoDownBtn = document.getElementById('servoDownBtn');
  const servoLeftBtn = document.getElementById('servoLeftBtn');
  const servoRightBtn = document.getElementById('servoRightBtn');
  const servoStopBtn = document.getElementById('servoStopBtn');
  const servoStepSlider = document.getElementById('servoStepSlider');
  const servoStepValue = document.getElementById('servoStepValue');
  const slamStartBtn = document.getElementById('slamStartBtn');
  const slamStopBtn = document.getElementById('slamStopBtn');
  const slamResetBtn = document.getElementById('slamResetBtn');
  const slamStatusEl = document.getElementById('slamStatus');
  const slamPoseEl = document.getElementById('slamPose');
  const slamStatsEl = document.getElementById('slamStats');
  const slamMapCanvas = document.getElementById('slamMapCanvas');
  const slamMapCtx = slamMapCanvas.getContext('2d');
  const slamCaptureCanvas = document.createElement('canvas');
  const slamCaptureCtx = slamCaptureCanvas.getContext('2d');

  const setStatus = (msg) => { statusEl.textContent = msg; };
  const setMqttStatus = (msg) => { mqttStatusEl.textContent = msg; };
  const setSlamStatus = (msg) => { slamStatusEl.textContent = `SLAM: ${msg}`; };
  const VIDEO_STALL_TIMEOUT_MS = 20000;
  const VIDEO_STALL_WINDOW_MS = 45000;
  const SLAM_FRAME_INTERVAL_MS = 250;
  const SLAM_STATE_INTERVAL_MS = 1000;

  function appendMqttLog(line) {
    const ts = new Date().toLocaleTimeString();
    mqttLogEl.textContent += `[${ts}] ${line}\n`;
    mqttLogEl.scrollTop = mqttLogEl.scrollHeight;
  }

  const janusServer = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/janus-ws`;
  const defaultMqttScheme = location.protocol === 'https:' ? 'wss' : 'ws';
  if (!mqttWsUrlEl.value.trim()) {
    mqttWsUrlEl.value = `${defaultMqttScheme}://${location.hostname}:9001`;
  }

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
    lastVideoRxMs = 0;
    videoStalled = false;
    stallWindowStartMs = 0;
    stallCountInWindow = 0;
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
        lastVideoRxMs = Date.now();
        videoStalled = false;
        stallWindowStartMs = 0;
        stallCountInWindow = 0;
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
        lastVideoRxMs = 0;
        videoStalled = false;
        stallWindowStartMs = 0;
        stallCountInWindow = 0;
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
        const prevBytes = prevVideoBytes;
        if (prevStatsTs > 0 && inboundVideo.timestamp > prevStatsTs) {
          const dtSec = (inboundVideo.timestamp - prevStatsTs) / 1000;
          const dBytes = inboundVideo.bytesReceived - prevBytes;
          bitrateKbps = dtSec > 0 ? (dBytes * 8) / dtSec / 1000 : 0;
        }
        prevVideoBytes = inboundVideo.bytesReceived;
        prevStatsTs = inboundVideo.timestamp;
        if (inboundVideo.bytesReceived > prevBytes) {
          lastVideoRxMs = Date.now();
          videoStalled = false;
        }
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

      if (!videoStalled && lastVideoRxMs > 0 && (Date.now() - lastVideoRxMs) > VIDEO_STALL_TIMEOUT_MS) {
        videoStalled = true;
        const errMsg = `Video stalled: no inbound video for ${Math.round(VIDEO_STALL_TIMEOUT_MS / 1000)}s`;
        console.error(`[webrtc] ${errMsg}`);
        const now = Date.now();
        if ((now - stallWindowStartMs) > VIDEO_STALL_WINDOW_MS) {
          stallWindowStartMs = now;
          stallCountInWindow = 0;
        }
        stallCountInWindow += 1;

        if (stallCountInWindow <= 2) {
          appendMqttLog(`WARN ${errMsg}; attempting local re-subscribe (${stallCountInWindow}/2)`);
          const roomId = parseInt(roomIdEl.value, 10);
          const roomPin = roomPinEl.value.trim();
          if (subscriberHandle) {
            subscriberHandle.detach();
            subscriberHandle = null;
          }
          selectedFeedId = null;
          if (Number.isFinite(roomId) && managerHandle) {
            requestAndAttachPublisher(roomId, roomPin);
            setStatus(`${errMsg}; recovering stream...`);
          } else {
            appendMqttLog('WARN local re-subscribe skipped: manager/room unavailable');
          }
          lastVideoRxMs = Date.now();
          videoStalled = false;
          return;
        }

        appendMqttLog(`ERROR ${errMsg}; restarting WebRTC session`);
        const cmdTopic = mqttCmdTopicEl.value.trim();
        if (mqttClient && mqttClient.connected && cmdTopic) {
          mqttClient.publish(cmdTopic, 'CLOSE_WEBRTC');
          appendMqttLog(`TX ${cmdTopic}: CLOSE_WEBRTC`);
        }
        await destroySession();
        setStatus(`${errMsg}; WebRTC restarted by controller.`);
        return;
      }
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

  function syncMqttFromRoom() {
    const roomId = roomIdEl.value.trim();
    const roomPin = roomPinEl.value.trim();
    if (roomId) {
      mqttUsernameEl.value = roomId;
      mqttMemTopicEl.value = `${roomId}/memory`;
      mqttCmdTopicEl.value = `${roomId}/command`;
    }
    mqttPasswordEl.value = roomPin;
  }

  async function fetchSlam(path, method = 'GET', body = null) {
    const opts = { method, headers: {} };
    if (body !== null) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    const resp = await fetch(path, opts);
    if (!resp.ok) {
      const text = await resp.text();
      throw new Error(`${resp.status} ${resp.statusText}: ${text}`);
    }
    return resp.json();
  }

  function updateSlamButtonState(isRunning) {
    slamStartBtn.disabled = isRunning;
    slamStopBtn.disabled = !isRunning;
  }

  function toNumber(value) {
    const n = Number(value);
    return Number.isFinite(n) ? n : null;
  }

  function formatN(value, digits = 3) {
    return Number.isFinite(value) ? value.toFixed(digits) : '-';
  }

  function yawFromQuaternionRad(pose) {
    const qx = toNumber(pose?.qx);
    const qy = toNumber(pose?.qy);
    const qz = toNumber(pose?.qz);
    const qw = toNumber(pose?.qw);
    if (qx === null || qy === null || qz === null || qw === null) {
      return null;
    }
    const siny = 2 * ((qw * qz) + (qx * qy));
    const cosy = 1 - (2 * ((qy * qy) + (qz * qz)));
    return Math.atan2(siny, cosy);
  }

  function resizeSlamCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(320, Math.floor(slamMapCanvas.clientWidth * dpr));
    const height = Math.max(180, Math.floor(slamMapCanvas.clientHeight * dpr));
    if (slamMapCanvas.width !== width || slamMapCanvas.height !== height) {
      slamMapCanvas.width = width;
      slamMapCanvas.height = height;
    }
  }

  function renderSlamMap(snapshot) {
    if (!slamMapCtx) {
      return;
    }
    resizeSlamCanvas();
    const width = slamMapCanvas.width;
    const height = slamMapCanvas.height;
    slamMapCtx.clearRect(0, 0, width, height);
    slamMapCtx.fillStyle = '#020617';
    slamMapCtx.fillRect(0, 0, width, height);

    const mapPoints = Array.isArray(snapshot?.map_points) ? snapshot.map_points : [];
    const trajectory = Array.isArray(snapshot?.trajectory) ? snapshot.trajectory : [];
    const mapSamples = [];
    const trajSamples = [];

    mapPoints.forEach((point) => {
      if (Array.isArray(point) && point.length >= 3) {
        const x = toNumber(point[0]);
        const z = toNumber(point[2]);
        if (x !== null && z !== null) {
          mapSamples.push({ x, z });
        }
      }
    });

    trajectory.forEach((pose) => {
      const x = toNumber(pose?.x);
      const z = toNumber(pose?.z);
      if (x !== null && z !== null) {
        trajSamples.push({ x, z });
      }
    });

    if (!mapSamples.length && !trajSamples.length) {
      slamMapCtx.fillStyle = '#64748b';
      slamMapCtx.font = `${Math.max(16, Math.floor(height * 0.05))}px sans-serif`;
      slamMapCtx.textAlign = 'center';
      slamMapCtx.fillText('No SLAM map yet', width / 2, height / 2);
      return;
    }

    const all = mapSamples.concat(trajSamples);
    let minX = all[0].x;
    let maxX = all[0].x;
    let minZ = all[0].z;
    let maxZ = all[0].z;
    all.forEach((p) => {
      minX = Math.min(minX, p.x);
      maxX = Math.max(maxX, p.x);
      minZ = Math.min(minZ, p.z);
      maxZ = Math.max(maxZ, p.z);
    });

    const spanX = Math.max(0.001, maxX - minX);
    const spanZ = Math.max(0.001, maxZ - minZ);
    const margin = Math.max(12, Math.floor(Math.min(width, height) * 0.05));
    const scale = Math.min((width - (2 * margin)) / spanX, (height - (2 * margin)) / spanZ);
    const offsetX = (width - (spanX * scale)) / 2;
    const offsetY = (height - (spanZ * scale)) / 2;
    const toCanvas = (x, z) => [offsetX + ((x - minX) * scale), height - (offsetY + ((z - minZ) * scale))];

    slamMapCtx.strokeStyle = 'rgba(71, 85, 105, 0.6)';
    slamMapCtx.lineWidth = 1;
    slamMapCtx.beginPath();
    slamMapCtx.moveTo(margin, height / 2);
    slamMapCtx.lineTo(width - margin, height / 2);
    slamMapCtx.moveTo(width / 2, margin);
    slamMapCtx.lineTo(width / 2, height - margin);
    slamMapCtx.stroke();

    slamMapCtx.fillStyle = 'rgba(148, 163, 184, 0.55)';
    mapSamples.forEach((p) => {
      const [cx, cy] = toCanvas(p.x, p.z);
      slamMapCtx.fillRect(cx - 1, cy - 1, 2, 2);
    });

    if (trajSamples.length >= 2) {
      slamMapCtx.strokeStyle = '#22d3ee';
      slamMapCtx.lineWidth = 2;
      slamMapCtx.beginPath();
      trajSamples.forEach((p, idx) => {
        const [cx, cy] = toCanvas(p.x, p.z);
        if (idx === 0) {
          slamMapCtx.moveTo(cx, cy);
        } else {
          slamMapCtx.lineTo(cx, cy);
        }
      });
      slamMapCtx.stroke();
    }

    if (trajSamples.length > 0) {
      const tail = trajSamples[trajSamples.length - 1];
      const [cx, cy] = toCanvas(tail.x, tail.z);
      slamMapCtx.fillStyle = '#ef4444';
      slamMapCtx.beginPath();
      slamMapCtx.arc(cx, cy, 5, 0, Math.PI * 2);
      slamMapCtx.fill();
    }
  }

  function updateSlamUi(snapshot) {
    slamLastSnapshot = snapshot;
    const state = snapshot?.state || 'unknown';
    const message = snapshot?.message || '';
    const statusText = message ? `${state} | ${message}` : state;
    setSlamStatus(statusText);

    slamRunning = Boolean(snapshot?.is_running);
    updateSlamButtonState(slamRunning);

    const pose = snapshot?.current_pose;
    const x = toNumber(pose?.x);
    const y = toNumber(pose?.y);
    const z = toNumber(pose?.z);
    const yawRad = yawFromQuaternionRad(pose);
    const yawDeg = yawRad === null ? null : (yawRad * 180.0 / Math.PI);
    if (x === null || y === null || z === null) {
      slamPoseEl.textContent = 'Pose: unavailable';
    } else {
      slamPoseEl.textContent = `Pose (x,y,z): ${formatN(x)} ${formatN(y)} ${formatN(z)} | Yaw: ${yawDeg === null ? '-' : yawDeg.toFixed(1)} deg`;
    }

    const frameCount = Number(snapshot?.frame_count || 0);
    const trackedFrames = Number(snapshot?.tracked_frames || 0);
    const lostFrames = Number(snapshot?.lost_frames || 0);
    const trajSize = Number(snapshot?.trajectory_size || 0);
    const mapSize = Number(snapshot?.map_points_size || 0);
    slamStatsEl.textContent = `Frames: ${frameCount} | Tracked: ${trackedFrames} | Lost: ${lostFrames} | Trajectory: ${trajSize} | Map points: ${mapSize}`;

    renderSlamMap(snapshot);
  }

  async function refreshSlamState(force = false) {
    const now = Date.now();
    if (!force && (now - lastSlamStateFetchMs) < 300) {
      return;
    }
    lastSlamStateFetchMs = now;
    try {
      const resp = await fetchSlam('/slam/api/state');
      if (resp?.slam) {
        updateSlamUi(resp.slam);
      }
    } catch (err) {
      setSlamStatus(`state error: ${err.message || err}`);
    }
  }

  async function pushVideoFrameToSlam() {
    if (!slamRunning || slamRequestInflight) {
      return;
    }
    if (!videoEl.srcObject || videoEl.readyState < 2 || videoEl.videoWidth < 2 || videoEl.videoHeight < 2) {
      return;
    }

    const sourceW = videoEl.videoWidth;
    const sourceH = videoEl.videoHeight;
    const maxW = 640;
    const scale = Math.min(1, maxW / sourceW);
    const targetW = Math.max(2, Math.round(sourceW * scale));
    const targetH = Math.max(2, Math.round(sourceH * scale));

    if (slamCaptureCanvas.width !== targetW || slamCaptureCanvas.height !== targetH) {
      slamCaptureCanvas.width = targetW;
      slamCaptureCanvas.height = targetH;
    }
    slamCaptureCtx.drawImage(videoEl, 0, 0, targetW, targetH);

    const imageBase64 = slamCaptureCanvas.toDataURL('image/jpeg', 0.72);
    slamRequestInflight = true;
    try {
      const resp = await fetchSlam('/slam/api/frame', 'POST', {
        image_base64: imageBase64,
        timestamp_ms: Date.now(),
      });
      if (resp?.slam) {
        updateSlamUi(resp.slam);
      }
    } catch (err) {
      setSlamStatus(`frame error: ${err.message || err}`);
    } finally {
      slamRequestInflight = false;
    }
  }

  async function slamStart() {
    try {
      setSlamStatus('starting...');
      const resp = await fetchSlam('/slam/api/control/start', 'POST');
      if (resp?.slam) {
        updateSlamUi(resp.slam);
      }
    } catch (err) {
      setSlamStatus(`start failed: ${err.message || err}`);
    }
  }

  async function slamStop() {
    try {
      const resp = await fetchSlam('/slam/api/control/stop', 'POST');
      if (resp?.slam) {
        updateSlamUi(resp.slam);
      }
    } catch (err) {
      setSlamStatus(`stop failed: ${err.message || err}`);
    }
  }

  async function slamReset() {
    try {
      const resp = await fetchSlam('/slam/api/control/reset', 'POST');
      if (resp?.slam) {
        updateSlamUi(resp.slam);
      }
    } catch (err) {
      setSlamStatus(`reset failed: ${err.message || err}`);
    }
  }

  function startSlamLoops() {
    if (!slamFrameTimer) {
      slamFrameTimer = setInterval(pushVideoFrameToSlam, SLAM_FRAME_INTERVAL_MS);
    }
    if (!slamStateTimer) {
      slamStateTimer = setInterval(() => {
        refreshSlamState(false);
      }, SLAM_STATE_INTERVAL_MS);
    }
  }

  function mqttConnect() {
    if (!window.mqtt) {
      setMqttStatus('MQTT: mqtt.js failed to load');
      return;
    }
    if (mqttClient) {
      mqttClient.end(true);
      mqttClient = null;
    }
    const wsUrl = mqttWsUrlEl.value.trim();
    const roomIdStr = document.getElementById('roomId').value.trim();
    const username = mqttUsernameEl.value.trim() || roomIdStr;
    const roomPin = document.getElementById('roomPin').value.trim();
    const password = mqttPasswordEl.value.trim() || roomPin;
    const memTopic = mqttMemTopicEl.value.trim() || `${roomIdStr}/memory`;
    const cmdTopic = mqttCmdTopicEl.value.trim() || `${roomIdStr}/command`;
    const statusTopic = `${roomIdStr}/status`;
    if (!wsUrl || !memTopic || !cmdTopic || !password) {
      setMqttStatus('MQTT: URL/topic/password is empty');
      return;
    }

    const clientId = `viewer_${Math.random().toString(16).slice(2, 10)}`;
    mqttClient = window.mqtt.connect(wsUrl, {
      clientId,
      clean: true,
      reconnectPeriod: 2000,
      connectTimeout: 5000,
      username,
      password,
    });
    setMqttStatus(`MQTT: connecting (${clientId})...`);
    appendMqttLog(`Connecting ${wsUrl}`);

    mqttClient.on('connect', () => {
      setMqttStatus(`MQTT: connected, subscribed ${memTopic}, ${statusTopic}`);
      mqttConnectBtn.disabled = true;
      mqttDisconnectBtn.disabled = false;
      mqttSendBtn.disabled = false;
      robotUpBtn.disabled = false;
      robotDownBtn.disabled = false;
      robotLeftBtn.disabled = false;
      robotRightBtn.disabled = false;
      robotStopBtn.disabled = false;
      robotRotateLeftBtn.disabled = false;
      robotRotateRightBtn.disabled = false;
      robotVelocitySlider.disabled = false;
      servoUpBtn.disabled = false;
      servoDownBtn.disabled = false;
      servoLeftBtn.disabled = false;
      servoRightBtn.disabled = false;
      servoStopBtn.disabled = false;
      servoStepSlider.disabled = false;
      mqttPublishCommand(`ROBOT_SPEED:${robotVelocitySlider.value}`);
      mqttPublishCommand(`SERVO_STEP:${servoStepSlider.value}`);
      mqttClient.subscribe(memTopic, { qos: 0 }, (err) => {
        if (err) {
          appendMqttLog(`Subscribe failed: ${err.message || err}`);
        } else {
          appendMqttLog(`Subscribed: ${memTopic}`);
        }
      });
      mqttClient.subscribe(statusTopic, { qos: 0 }, (err) => {
        if (err) {
          appendMqttLog(`Subscribe failed: ${err.message || err}`);
        } else {
          appendMqttLog(`Subscribed: ${statusTopic}`);
        }
      });
      pendingWebrtcOpen = true;
      mqttClient.publish(cmdTopic, 'OPEN_WEBRTC');
      appendMqttLog(`TX ${cmdTopic}: OPEN_WEBRTC`);
      if (heartbeatTimer) {
        clearInterval(heartbeatTimer);
      }
      heartbeatTimer = setInterval(() => {
        if (mqttClient && mqttClient.connected) {
          mqttClient.publish(cmdTopic, 'HEARTBEAT');
        }
      }, 3000);
    });
    mqttClient.on('reconnect', () => setMqttStatus('MQTT: reconnecting...'));
    mqttClient.on('close', () => {
      setMqttStatus('MQTT: disconnected');
      mqttConnectBtn.disabled = false;
      mqttDisconnectBtn.disabled = true;
      mqttSendBtn.disabled = true;
      robotUpBtn.disabled = true;
      robotDownBtn.disabled = true;
      robotLeftBtn.disabled = true;
      robotRightBtn.disabled = true;
      robotStopBtn.disabled = true;
      robotRotateLeftBtn.disabled = true;
      robotRotateRightBtn.disabled = true;
      robotVelocitySlider.disabled = true;
      servoUpBtn.disabled = true;
      servoDownBtn.disabled = true;
      servoLeftBtn.disabled = true;
      servoRightBtn.disabled = true;
      servoStopBtn.disabled = true;
      servoStepSlider.disabled = true;
      pendingWebrtcOpen = false;
      if (robotSpeedPublishTimer) {
        clearTimeout(robotSpeedPublishTimer);
        robotSpeedPublishTimer = null;
      }
      pendingRobotSpeedValue = null;
      if (heartbeatTimer) {
        clearInterval(heartbeatTimer);
        heartbeatTimer = null;
      }
      if (janus) {
        destroySession();
      }
    });
    mqttClient.on('error', (err) => {
      appendMqttLog(`Error: ${err.message || err}`);
    });
    mqttClient.on('message', (topic, payload) => {
      const text = payload ? payload.toString() : '';
      appendMqttLog(`RX ${topic}: ${text}`);
      try {
        const msg = JSON.parse(text);
        if (topic.endsWith('/status') && msg.webrtc === 'ready' && pendingWebrtcOpen) {
          pendingWebrtcOpen = false;
          if (!janus) {
            connect();
          }
        }
      } catch (_e) {
      }
    });
  }

  function mqttDisconnect() {
    const cmdTopic = mqttCmdTopicEl.value.trim();
    if (mqttClient && mqttClient.connected && cmdTopic) {
      mqttClient.publish(cmdTopic, 'CLOSE_WEBRTC');
      appendMqttLog(`TX ${cmdTopic}: CLOSE_WEBRTC`);
    }
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer);
      heartbeatTimer = null;
    }
    if (mqttClient) {
      mqttClient.end(true);
      mqttClient = null;
    }
    if (robotSpeedPublishTimer) {
      clearTimeout(robotSpeedPublishTimer);
      robotSpeedPublishTimer = null;
    }
    pendingRobotSpeedValue = null;
    mqttConnectBtn.disabled = false;
    mqttDisconnectBtn.disabled = true;
    mqttSendBtn.disabled = true;
    robotUpBtn.disabled = true;
    robotDownBtn.disabled = true;
    robotLeftBtn.disabled = true;
    robotRightBtn.disabled = true;
    robotStopBtn.disabled = true;
    robotRotateLeftBtn.disabled = true;
    robotRotateRightBtn.disabled = true;
    robotVelocitySlider.disabled = true;
    servoUpBtn.disabled = true;
    servoDownBtn.disabled = true;
    servoLeftBtn.disabled = true;
    servoRightBtn.disabled = true;
    servoStopBtn.disabled = true;
    servoStepSlider.disabled = true;
    setMqttStatus('MQTT: disconnected');
  }

  function mqttSendCommand() {
    if (!mqttClient || !mqttClient.connected) {
      setMqttStatus('MQTT: not connected');
      return;
    }
    const cmdTopic = mqttCmdTopicEl.value.trim();
    const cmd = mqttCmdInputEl.value.trim();
    if (!cmd) {
      setMqttStatus('MQTT: command is empty');
      return;
    }
    mqttClient.publish(cmdTopic, cmd, { qos: 0, retain: false }, (err) => {
      if (err) {
        appendMqttLog(`TX failed: ${err.message || err}`);
      } else {
        appendMqttLog(`TX ${cmdTopic}: ${cmd}`);
      }
    });
  }

  function mqttPublishCommand(cmd) {
    if (!mqttClient || !mqttClient.connected) {
      setMqttStatus('MQTT: not connected');
      return false;
    }
    const cmdTopic = mqttCmdTopicEl.value.trim();
    if (!cmdTopic) {
      setMqttStatus('MQTT: command topic is empty');
      return false;
    }
    mqttClient.publish(cmdTopic, cmd, { qos: 0, retain: false }, (err) => {
      if (err) {
        appendMqttLog(`TX failed: ${err.message || err}`);
      } else {
        appendMqttLog(`TX ${cmdTopic}: ${cmd}`);
      }
    });
    return true;
  }

  function installButtonPressEffects() {
    const buttons = document.querySelectorAll('button');
    buttons.forEach((btn) => {
      const press = () => {
        if (!btn.disabled) {
          btn.classList.add('btn-pressed');
        }
      };
      const release = () => btn.classList.remove('btn-pressed');
      btn.addEventListener('pointerdown', press);
      btn.addEventListener('pointerup', release);
      btn.addEventListener('pointercancel', release);
      btn.addEventListener('pointerleave', release);
      btn.addEventListener('blur', release);
    });
  }

  function publishRobotSpeed(speedValue, forceImmediate = false) {
    if (!mqttClient || !mqttClient.connected) {
      return;
    }
    const now = Date.now();
    const throttleMs = 120;
    const publish = (value) => {
      mqttPublishCommand(`ROBOT_SPEED:${value}`);
      lastRobotSpeedPublishMs = Date.now();
    };

    if (forceImmediate || (now - lastRobotSpeedPublishMs) >= throttleMs) {
      if (robotSpeedPublishTimer) {
        clearTimeout(robotSpeedPublishTimer);
        robotSpeedPublishTimer = null;
      }
      pendingRobotSpeedValue = null;
      publish(speedValue);
      return;
    }

    pendingRobotSpeedValue = speedValue;
    if (!robotSpeedPublishTimer) {
      robotSpeedPublishTimer = setTimeout(() => {
        robotSpeedPublishTimer = null;
        if (pendingRobotSpeedValue !== null) {
          publish(pendingRobotSpeedValue);
          pendingRobotSpeedValue = null;
        }
      }, throttleMs - (now - lastRobotSpeedPublishMs));
    }
  }

  function bindControlButton(btn, cmd, stopCmd) {
    const send = (ev) => {
      ev.preventDefault();
      mqttPublishCommand(cmd);
    };
    const stop = (ev) => {
      ev.preventDefault();
      mqttPublishCommand(stopCmd);
    };
    btn.addEventListener('mousedown', send);
    btn.addEventListener('touchstart', send, { passive: false });
    btn.addEventListener('mouseup', stop);
    btn.addEventListener('mouseleave', stop);
    btn.addEventListener('touchend', stop, { passive: false });
    btn.addEventListener('touchcancel', stop, { passive: false });
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
    const cmdTopic = mqttCmdTopicEl.value.trim();
    if (mqttClient && mqttClient.connected && cmdTopic) {
      mqttClient.publish(cmdTopic, 'CLOSE_WEBRTC');
      appendMqttLog(`TX ${cmdTopic}: CLOSE_WEBRTC`);
    }
    await destroySession();
    setStatus('Disconnected.');
  });
  slamStartBtn.addEventListener('click', slamStart);
  slamStopBtn.addEventListener('click', slamStop);
  slamResetBtn.addEventListener('click', slamReset);
  mqttConnectBtn.addEventListener('click', mqttConnect);
  mqttDisconnectBtn.addEventListener('click', mqttDisconnect);
  mqttSendBtn.addEventListener('click', mqttSendCommand);
  mqttCmdInputEl.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') {
      mqttSendCommand();
    }
  });
  roomIdEl.addEventListener('input', syncMqttFromRoom);
  roomPinEl.addEventListener('input', syncMqttFromRoom);
  servoStepValue.textContent = servoStepSlider.value;
  servoStepSlider.addEventListener('input', () => {
    servoStepValue.textContent = servoStepSlider.value;
  });
  servoStepSlider.addEventListener('change', () => {
    mqttPublishCommand(`SERVO_STEP:${servoStepSlider.value}`);
  });
  bindControlButton(robotUpBtn, 'ROBOT_UP', 'ROBOT_STOP');
  bindControlButton(robotDownBtn, 'ROBOT_DOWN', 'ROBOT_STOP');
  bindControlButton(robotLeftBtn, 'ROBOT_LEFT', 'ROBOT_STOP');
  bindControlButton(robotRightBtn, 'ROBOT_RIGHT', 'ROBOT_STOP');
  bindControlButton(robotRotateLeftBtn, 'ROBOT_ROTATE_LEFT', 'ROBOT_STOP');
  bindControlButton(robotRotateRightBtn, 'ROBOT_ROTATE_RIGHT', 'ROBOT_STOP');
  robotStopBtn.addEventListener('click', (ev) => {
    ev.preventDefault();
    mqttPublishCommand('ROBOT_STOP');
  });
  robotVelocityValue.textContent = robotVelocitySlider.value;
  robotVelocitySlider.addEventListener('input', () => {
    robotVelocityValue.textContent = robotVelocitySlider.value;
    publishRobotSpeed(robotVelocitySlider.value, false);
  });
  robotVelocitySlider.addEventListener('change', () => {
    publishRobotSpeed(robotVelocitySlider.value, true);
  });
  bindControlButton(servoUpBtn, 'SERVO_UP', 'SERVO_STOP');
  bindControlButton(servoDownBtn, 'SERVO_DOWN', 'SERVO_STOP');
  bindControlButton(servoLeftBtn, 'SERVO_LEFT', 'SERVO_STOP');
  bindControlButton(servoRightBtn, 'SERVO_RIGHT', 'SERVO_STOP');
  servoStopBtn.addEventListener('click', (ev) => {
    ev.preventDefault();
    mqttPublishCommand('SERVO_STOP');
  });
  window.addEventListener('resize', () => {
    if (slamLastSnapshot) {
      renderSlamMap(slamLastSnapshot);
    } else {
      renderSlamMap({});
    }
  });
  installButtonPressEffects();
  syncMqttFromRoom();
  updateSlamButtonState(false);
  renderSlamMap({});
  startSlamLoops();
  refreshSlamState(true);
})();
