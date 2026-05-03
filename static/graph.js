// Wait for fonts to load before initializing chart (fixes custom font rendering)
document.fonts.ready.then(() => {

// Plugin: styled text + CSS variable integration for OBS Custom CSS control
const styledTextPlugin = {
    id: 'styledText',
    beforeDraw(chart) {
        const style = getComputedStyle(document.body);

        // Re-read font from CSS every frame so OBS Custom CSS overrides work
        const fontFamily = style.fontFamily;
        chart.options.scales.x.ticks.font.family = fontFamily;
        chart.options.scales.y.ticks.font.family = fontFamily;

        // Read line color and smoothing from CSS variables
        const lineColor = style.getPropertyValue('--graph-line-color').trim();
        if (lineColor) {
            chart.data.datasets[0].borderColor = lineColor;
        }
        const smoothing = style.getPropertyValue('--graph-line-smoothing').trim();
        if (smoothing) {
            chart.data.datasets[0].tension = parseFloat(smoothing);
        }

        // Read axis label visibility
        const showX = style.getPropertyValue('--graph-show-x-labels').trim();
        const showY = style.getPropertyValue('--graph-show-y-labels').trim();
        chart.options.scales.x.ticks.display = showX !== '0';
        chart.options.scales.y.ticks.display = showY !== '0';

        // Read text styling from CSS variables
        const textColor = style.getPropertyValue('--graph-text-color').trim() || '#fff';
        const outlineColor = style.getPropertyValue('--graph-text-outline-color').trim() || '#000';
        const outlineWidth = parseFloat(style.getPropertyValue('--graph-text-outline-width').trim()) || 4;
        chart.options.scales.x.ticks.color = textColor;
        chart.options.scales.y.ticks.color = textColor;

        // Monkey-patch fillText for outline + drop shadow on all canvas text.
        // Stored on ctx so afterDraw can restore.
        const ctx = chart.ctx;
        // If a previous draw threw before afterDraw ran, ctx.fillText is
        // still pointing at the wrapper. Undo that first so we don't
        // wrap-our-own-wrapper.
        if (ctx.__origFillText) {
            ctx.fillText = ctx.__origFillText;
            delete ctx.__origFillText;
        }
        const origFill = CanvasRenderingContext2D.prototype.fillText;
        ctx.__origFillText = origFill;
        ctx.fillText = function(text, x, y, maxWidth) {
            const args = maxWidth !== undefined
                ? [text, x, y, maxWidth] : [text, x, y];
            // 1. Outline stroke
            this.save();
            try {
                this.strokeStyle = outlineColor;
                this.lineWidth = outlineWidth;
                this.lineJoin = 'round';
                CanvasRenderingContext2D.prototype.strokeText.apply(this, args);
            } finally {
                this.restore();
            }
            // 2. Fill with drop shadow
            this.save();
            try {
                this.shadowColor = 'rgba(0, 0, 0, 0.6)';
                this.shadowBlur = 4;
                this.shadowOffsetX = 1;
                this.shadowOffsetY = 1;
                origFill.apply(this, args);
            } finally {
                this.restore();
            }
        };
    },
    afterDraw(chart) {
        const ctx = chart.ctx;
        if (ctx.__origFillText) {
            ctx.fillText = ctx.__origFillText;
            delete ctx.__origFillText;
        }
    }
};

const ctx = document.getElementById('checks-chart').getContext('2d');

const chart = new Chart(ctx, {
    type: 'line',
    plugins: [styledTextPlugin],
    data: {
        labels: [],
        datasets: [{
            data: [],
            borderColor: 'rgba(75, 192, 192, 1)',
            borderWidth: 2,
            fill: false,
            tension: 0,
            pointRadius: 0,
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: { duration: 300 },
        plugins: {
            legend: { display: false }
        },
        scales: {
            x: {
                ticks: {
                    color: '#fff',
                    font: { size: 20, weight: 'bold', family: '' },
                    maxRotation: 0,
                    autoSkip: true,
                    autoSkipPadding: 20
                },
                grid: { display: false },
                border: { display: false }
            },
            y: {
                beginAtZero: true,
                ticks: {
                    color: '#fff',
                    font: { size: 20, weight: 'bold', family: '' },
                    maxTicksLimit: 6
                },
                grid: { display: false },
                border: { display: false }
            }
        }
    }
});

const MAX_DISPLAY_POINTS = 500;

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${protocol}//${window.location.host}/graph${window.location.search}`;

let socket = null;

function connect() {
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        console.log('Connected to Axolotl graph endpoint.');
    };

    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'graph_update') {
                addPoint(data);
            } else if (data.type === 'graph_history') {
                loadHistory(data);
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

function formatTime(ts) {
    const d = new Date(ts * 1000);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function downsample(points, maxPoints) {
    if (points.length <= maxPoints) return points;
    const result = [];
    const stride = (points.length - 1) / (maxPoints - 1);
    for (let i = 0; i < maxPoints; i++) {
        const idx = Math.floor(i * stride);
        result.push(points[idx]);
    }
    return result;
}

function loadHistory(data) {
    const sampled = downsample(data.points, MAX_DISPLAY_POINTS);
    chart.data.labels = sampled.map(p => formatTime(p.timestamp));
    chart.data.datasets[0].data = sampled.map(p => p.checked);
    chart.update();
}

function addPoint(data) {
    chart.data.labels.push(formatTime(data.timestamp));
    chart.data.datasets[0].data.push(data.checked);

    if (chart.data.labels.length > MAX_DISPLAY_POINTS) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
    }
    chart.update();
}

connect();

}); // end document.fonts.ready
