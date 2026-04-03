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
let messageQueue = [];
let isProcessingQueue = false;

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${protocol}//${window.location.host}/feed${window.location.search}`;

let socket = null;

function connect() {
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        console.log('Connected to Axolotl feed.');
    };

    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'feed_item') {
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
    el.className = 'feed-item ' + (data.category || 'system');

    const timestampEl = document.createElement('span');
    timestampEl.className = 'timestamp';
    timestampEl.innerText = getTimestamp() + ' ';

    const textEl = document.createElement('div');
    textEl.className = 'feed-text';
    textEl.innerHTML = data.html || data.text || '';
    textEl.prepend(timestampEl);

    el.appendChild(textEl);
    feedContainer.appendChild(el);

    while (feedContainer.children.length > maxItems) {
        feedContainer.removeChild(feedContainer.firstChild);
    }
}

connect();
