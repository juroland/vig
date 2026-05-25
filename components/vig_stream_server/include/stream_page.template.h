#pragma once

static const char stream_page_html[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>VisionLink | Live Intelligence</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&family=Outfit:wght@700&display=swap" rel="stylesheet">
    <style>
        :root {
            --primary: #6366f1;
            --primary-glow: rgba(99, 102, 241, 0.5);
            --secondary: #a855f7;
            --bg: #0f172a;
            --card: rgba(30, 41, 59, 0.7);
            --border: rgba(255, 255, 255, 0.1);
            --text: #f8fafc;
            --text-dim: #94a3b8;
            --success: #10b981;
            --danger: #ef4444;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
            user-select: none;
        }

        body {
            background-color: var(--bg);
            background-image:
                radial-gradient(at 0% 0%, rgba(99, 102, 241, 0.15) 0px, transparent 50%),
                radial-gradient(at 100% 100%, rgba(168, 85, 247, 0.15) 0px, transparent 50%);
            color: var(--text);
            font-family: 'Inter', sans-serif;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 40px 20px;
        }

        .header {
            margin-bottom: 32px;
            text-align: center;
            animation: fadeInDown 0.8s ease-out;
        }

        .header h1 {
            font-family: 'Outfit', sans-serif;
            font-size: 2.5rem;
            background: linear-gradient(135deg, #fff 0%, #cbd5e1 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            margin-bottom: 8px;
        }

        .header p {
            color: var(--text-dim);
            font-size: 1rem;
            letter-spacing: 0.05em;
            text-transform: uppercase;
        }

        .main-container {
            width: 100%;
            max-width: 960px;
            position: relative;
            animation: fadeInUp 0.8s ease-out;
        }

        .video-card {
            background: var(--card);
            backdrop-filter: blur(20px);
            border: 1px solid var(--border);
            border-radius: 24px;
            padding: 12px;
            box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5);
            position: relative;
            overflow: hidden;
        }

        .video-wrapper {
            width: 100%;
            aspect-ratio: 16 / 9;
            background: #000;
            border-radius: 16px;
            overflow: hidden;
            position: relative;
            box-shadow: inset 0 0 40px rgba(0,0,0,0.9);
        }

        video {
            width: 100%;
            height: 100%;
            object-fit: cover;
            display: block;
        }

        .overlay-status {
            position: absolute;
            top: 24px;
            left: 24px;
            display: flex;
            gap: 12px;
            z-index: 10;
        }

        .badge {
            padding: 6px 12px;
            border-radius: 8px;
            font-size: 0.75rem;
            font-weight: 700;
            text-transform: uppercase;
            display: flex;
            align-items: center;
            gap: 8px;
            backdrop-filter: blur(8px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }

        .badge-live {
            background: rgba(16, 185, 129, 0.2);
            color: var(--success);
        }

        .badge-live .pulse {
            width: 8px;
            height: 8px;
            background: var(--success);
            border-radius: 50%;
            animation: pulse 2s infinite;
        }

        .badge-disconnected {
            background: rgba(239, 68, 68, 0.2);
            color: var(--danger);
        }

        .badge-info {
            background: rgba(255, 255, 255, 0.1);
            color: #fff;
        }

        .controls {
            margin-top: 24px;
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 16px;
        }

        .stat-box {
            background: var(--card);
            border: 1px solid var(--border);
            border-radius: 16px;
            padding: 16px;
            text-align: center;
            transition: transform 0.3s ease, background 0.3s ease;
        }

        .stat-box:hover {
            transform: translateY(-4px);
            background: rgba(255, 255, 255, 0.05);
        }

        .stat-label {
            display: block;
            color: var(--text-dim);
            font-size: 0.75rem;
            text-transform: uppercase;
            margin-bottom: 4px;
        }

        .stat-value {
            font-family: 'Outfit', sans-serif;
            font-size: 1.5rem;
            color: #fff;
        }

        .loading-overlay {
            position: absolute;
            inset: 0;
            background: var(--bg);
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            z-index: 20;
            transition: opacity 0.5s ease;
        }

        .loading-overlay.hidden {
            opacity: 0;
            pointer-events: none;
        }

        .spinner {
            width: 48px;
            height: 48px;
            border: 3px solid rgba(99, 102, 241, 0.1);
            border-top: 3px solid var(--primary);
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }

        @keyframes pulse {
            0% { transform: scale(1); opacity: 1; box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.4); }
            70% { transform: scale(1.2); opacity: 0.7; box-shadow: 0 0 0 10px rgba(16, 185, 129, 0); }
            100% { transform: scale(1); opacity: 1; box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
        }

        @keyframes spin {
            to { transform: rotate(360deg); }
        }

        @keyframes fadeInUp {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }

        @keyframes fadeInDown {
            from { opacity: 0; transform: translateY(-20px); }
            to { opacity: 1; transform: translateY(0); }
        }

        #error-msg {
            color: var(--danger);
            margin-top: 12px;
            font-size: 0.875rem;
            display: none;
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>VisionLink P4</h1>
        <p>Industrial Edge AI Node</p>
    </div>

    <div class="main-container">
        <div class="video-card">
            <div id="loading" class="loading-overlay">
                <div class="spinner"></div>
                <p style="margin-top: 16px; font-weight: 600;">Establishing Secure Link...</p>
                <div id="error-msg">Connection failed. Retrying...</div>
            </div>

            <div class="overlay-status">
                <div id="status-badge" class="badge badge-disconnected">
                    <div id="status-pulse"></div>
                    <span id="status-text">Offline</span>
                </div>
                <div class="badge badge-info">1920x1080 @ 30FPS</div>
            </div>

            <div class="video-wrapper">
                <video id="player" autoplay muted playsinline></video>
            </div>
        </div>

        <div class="controls">
            <div class="stat-box">
                <span class="stat-label">Frame Rate</span>
                <span class="stat-value"><span id="fps-val">0.0</span> <span style="font-size: 0.8rem; color: var(--text-dim)">FPS</span></span>
            </div>
            <div class="stat-box">
                <span class="stat-label">Link Status</span>
                <span class="stat-value" id="bitrate-val">STABLE</span>
            </div>
            <div class="stat-box">
                <span class="stat-label">Latency</span>
                <span class="stat-value"><span id="latency-val">--</span> <span style="font-size: 0.8rem; color: var(--text-dim)">MS</span></span>
            </div>
        </div>
    </div>

    <script>
        /* JMUXER_PLACEHOLDER */

        // Application Logic
        const playerNode = document.getElementById('player');
        const loadingOverlay = document.getElementById('loading');
        const statusBadge = document.getElementById('status-badge');
        const statusText = document.getElementById('status-text');
        const errorMsg = document.getElementById('error-msg');
        const fpsVal = document.getElementById('fps-val');
        const latencyVal = document.getElementById('latency-val');

        let jmuxer;
        let ws;
        let frameCount = 0;
        let lastFpsUpdate = Date.now();
        let reconnectTimeout;

        function initPlayer() {
            jmuxer = new JMuxer({
                node: 'player',
                mode: 'video',
                flushingTime: 0, // Minimize latency
                debug: false
            });

            // Smooth dynamic playbackRate adjustment for ultra-low latency
            setInterval(() => {
                if (playerNode && playerNode.buffered.length > 0) {
                    const bufferEnd = playerNode.buffered.end(playerNode.buffered.length - 1);
                    const delay = bufferEnd - playerNode.currentTime;

                    if (delay > 0.35) {
                        // Hard catch-up: skip directly to the newest frame
                        playerNode.currentTime = bufferEnd - 0.02;
                        playerNode.playbackRate = 1.0;
                    } else if (delay > 0.15) {
                        // Smooth catch-up: speed up slightly to clear backlog
                        playerNode.playbackRate = 1.25;
                    } else if (delay < 0.06) {
                        // Anti-starvation: slow down slightly so we don't hit the buffer wall
                        playerNode.playbackRate = 0.95;
                    } else {
                        // Target latency reached
                        playerNode.playbackRate = 1.0;
                    }
                }
            }, 50);
        }

        function connect() {
            if (ws) ws.close();

            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const wsUrl = `${protocol}//${window.location.host}/stream`;

            ws = new WebSocket(wsUrl);
            ws.binaryType = 'arraybuffer';

            ws.onopen = () => {
                console.log('Connected to VisionLink Stream');
                statusBadge.className = 'badge badge-live';
                statusText.innerText = 'Live';
                loadingOverlay.classList.add('hidden');
                errorMsg.style.display = 'none';
                if (!jmuxer) initPlayer();

                // Trigger the ESP32 ws_handler by sending a HELO message
                ws.send('HELO');
            };

            ws.onmessage = (event) => {
                if (event.data instanceof ArrayBuffer) {
                    jmuxer.feed({
                        video: new Uint8Array(event.data)
                    });

                    frameCount++;
                    const now = Date.now();
                    if (now - lastFpsUpdate >= 1000) {
                        const fps = (frameCount * 1000) / (now - lastFpsUpdate);
                        fpsVal.innerText = fps.toFixed(1);
                        frameCount = 0;
                        lastFpsUpdate = now;
                    }

                    // Display actual calculated latency in milliseconds
                    if (playerNode.buffered.length > 0) {
                        const bufferEnd = playerNode.buffered.end(playerNode.buffered.length - 1);
                        const delay = bufferEnd - playerNode.currentTime;
                        latencyVal.innerText = Math.max(0, Math.round(delay * 1000));
                    }
                }
            };

            ws.onclose = () => {
                statusBadge.className = 'badge badge-disconnected';
                statusText.innerText = 'Disconnected';
                loadingOverlay.classList.remove('hidden');
                errorMsg.style.display = 'block';

                // Attempt reconnect
                clearTimeout(reconnectTimeout);
                reconnectTimeout = setTimeout(connect, 2000);
            };

            ws.onerror = (err) => {
                console.error('WebSocket Error:', err);
            };
        }

        // Initialize
        document.addEventListener('DOMContentLoaded', connect);

        // Auto-play fix for browsers
        document.body.addEventListener('click', () => {
            if (playerNode.paused) playerNode.play();
        }, { once: true });
    </script>
</body>
</html>
)HTML";
