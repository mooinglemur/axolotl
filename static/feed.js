const feedContainer = document.getElementById('feed-container');
const maxItems = 20;

function getTimestamp() {
    const now = new Date();
    const h = String(now.getHours()).padStart(2, '0');
    const m = String(now.getMinutes()).padStart(2, '0');
    const s = String(now.getSeconds()).padStart(2, '0');
    return `[${h}:${m}:${s}]`;
}

const urlParams = new URLSearchParams(window.location.search);
const messageDelay = parseInt(urlParams.get('delay')) || 0;
const excludeFiller = urlParams.get('excludefiller') === '1';
let messageQueue = [];
let isProcessingQueue = false;

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${protocol}//${window.location.host}/feed${window.location.search}`;

let socket = null;
let hasConnectedBefore = false;
let isConnected = false;

function addStatusMessage(text) {
    const el = document.createElement('div');
    el.className = 'feed-item system';
    const textEl = document.createElement('div');
    textEl.className = 'feed-text';
    const timestampEl = document.createElement('span');
    timestampEl.className = 'timestamp';
    timestampEl.innerText = getTimestamp() + ' ';
    textEl.appendChild(timestampEl);
    textEl.appendChild(document.createTextNode(text));
    el.appendChild(textEl);
    feedContainer.appendChild(el);
    while (feedContainer.children.length > maxItems) {
        feedContainer.removeChild(feedContainer.firstChild);
    }
}

function connect() {
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        console.log('Connected to Axolotl feed.');
        isConnected = true;
        addStatusMessage(hasConnectedBefore ? 'Reconnected to Axolotl.' : 'Connected to Axolotl.');
        hasConnectedBefore = true;
    };

    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'feed_item') {
                if (excludeFiller && data.flags === 0 && (data.category === 'player-self' || data.category === 'player-other')) {
                    return;
                }
                if (messageDelay > 0) {
                    messageQueue.push(data);
                    if (!isProcessingQueue) {
                        processQueue();
                    }
                } else {
                    addFeedItem(data);
                }
            }
        } catch (e) {
            console.error('Error parsing message:', e);
        }
    };

    socket.onclose = () => {
        console.log('Disconnected. Reconnecting in 5s...');
        if (isConnected) {
            isConnected = false;
            addStatusMessage('Disconnected from Axolotl. Will attempt to reconnect.');
        }
        setTimeout(connect, 5000);
    };

    socket.onerror = (err) => {
        console.error('WebSocket error:', err);
        socket.close();
    };
}

function processQueue() {
    if (messageQueue.length === 0) {
        isProcessingQueue = false;
        return;
    }
    isProcessingQueue = true;
    const data = messageQueue.shift();
    addFeedItem(data);
    setTimeout(processQueue, messageDelay);
}

function addFeedItem(data) {
    const el = document.createElement('div');
    // category comes from our own server code — restrict to safe identifier chars
    const safeCategory = (data.category || 'system').replace(/[^a-zA-Z0-9_-]/g, '');
    el.className = 'feed-item ' + safeCategory;

    const timestampEl = document.createElement('span');
    timestampEl.className = 'timestamp';
    timestampEl.innerText = getTimestamp() + ' ';

    const textEl = document.createElement('div');
    textEl.className = 'feed-text';
    textEl.appendChild(timestampEl);

    for (const part of (data.parts || [])) {
        if (part.class || part.style) {
            const span = document.createElement('span');
            if (part.class) span.className = part.class;
            if (part.style) span.setAttribute('style', part.style);
            span.textContent = part.text || '';
            textEl.appendChild(span);
        } else {
            textEl.appendChild(document.createTextNode(part.text || ''));
        }
    }

    el.appendChild(textEl);
    feedContainer.appendChild(el);

    while (feedContainer.children.length > maxItems) {
        feedContainer.removeChild(feedContainer.firstChild);
    }
}

connect();
