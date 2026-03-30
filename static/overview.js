const checkedLocationsEl = document.getElementById('checked-locations');
const totalLocationsEl = document.getElementById('total-locations');
const completedGamesEl = document.getElementById('completed-games');
const totalGamesEl = document.getElementById('total-games');
const progressBarFill = document.getElementById('progress-bar-fill');

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${protocol}//${window.location.host}/overview${window.location.search}`;

let socket = null;

function connect() {
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        console.log('Connected to Axolotl overview endpoint.');
    };

    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'overview_update') {
                updateOverview(data);
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

function updateOverview(data) {
    if (data.total_locations !== undefined) {
        totalLocationsEl.innerText = data.total_locations;
    }
    if (data.checked_locations !== undefined) {
        checkedLocationsEl.innerText = data.checked_locations;
    }
    if (data.total_games !== undefined) {
        totalGamesEl.innerText = data.total_games;
    }
    if (data.completed_games !== undefined) {
        completedGamesEl.innerText = data.completed_games;
    }

    // Update progress bar
    if (data.total_locations > 0 && data.checked_locations !== undefined) {
        let pct = (data.checked_locations / data.total_locations) * 100;
        pct = Math.min(Math.max(pct, 0), 100);
        progressBarFill.style.width = `${pct}%`;
        document.getElementById('progress-percent').innerText = `${pct.toFixed(1)}%`;
    } else {
        progressBarFill.style.width = `0%`;
        document.getElementById('progress-percent').innerText = `0.0%`;
    }
}

connect();
