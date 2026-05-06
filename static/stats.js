// Axolotl /stats overlay client.
// Receives `stats_snapshot` and `goal_event` messages over /stats WebSocket.
// Renders three independent sections; each is gated by CSS visibility, so
// OBS users hide what they don't want via Custom CSS.
(() => {

const params = new URLSearchParams(window.location.search);

const mineMode = (params.get('mine') || 'all').toLowerCase();   // all | cycle | latest
const playersOverride = (params.get('players') || '')
    .split(',').map(s => s.trim()).filter(s => s.length > 0);
const eligibleNotable = new Set(
    (params.get('notable') || 'ahead,behind,idle,not_started')
        .split(',').map(s => s.trim().toLowerCase()).filter(s => s.length > 0));
// How many "ahead" cards to expose. 1 → just "Most ahead". 2+ → adds
// "2nd most ahead", "3rd most ahead", etc. as separate rotation cards.
const showTopAhead = Math.max(1, Math.min(99,
    parseInt(params.get('showtop') || '1', 10) || 1));

// ---- WebSocket plumbing ------------------------------------------------

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${protocol}//${window.location.host}/stats${window.location.search}`;

let socket = null;
let lastSnapshot = null;

function connect() {
    socket = new WebSocket(wsUrl);
    socket.onopen = () => console.log('Connected to Axolotl stats endpoint.');
    socket.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'stats_snapshot') {
                lastSnapshot = data;
                onSnapshot(data);
            } else if (data.type === 'goal_event') {
                onGoalEvent(data);
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

// ---- CSS-variable readers ----------------------------------------------

function readNumberVar(name, fallback) {
    const v = parseFloat(getComputedStyle(document.body).getPropertyValue(name));
    return isFinite(v) ? v : fallback;
}

// CSS variable values used in `content: var(...)` are typically wrapped
// in quotes; strip them so we can plug the value into textContent.
function readLabelVar(name, fallback) {
    let v = getComputedStyle(document.body).getPropertyValue(name).trim();
    if ((v.startsWith('"') && v.endsWith('"')) ||
        (v.startsWith("'") && v.endsWith("'"))) {
        v = v.slice(1, -1);
    }
    return v || fallback;
}

// Translate `--stats-show-*: 0` CSS variables into body classes so they
// take effect even when set via a <style> block (OBS Custom CSS), not
// just an inline style attribute.
function syncVisibilityClasses() {
    const cs = getComputedStyle(document.body);
    const isOff = (name) => cs.getPropertyValue(name).trim() === '0';
    document.body.classList.toggle('hide-game',    isOff('--stats-show-game'));
    document.body.classList.toggle('hide-percent', isOff('--stats-show-percent'));
    document.body.classList.toggle('hide-counts',  isOff('--stats-show-counts'));
    document.body.classList.toggle('no-goal-fireworks',
                                   isOff('--stats-goal-fireworks'));
}

// Build the heading text for a notable card. Reads the --label-* CSS
// variables so OBS Custom CSS can localize them without touching JS.
function computeCardLabel(card) {
    if (card.kind === 'ahead') {
        const rank = card.rank || 1;
        if (rank === 1) return readLabelVar('--label-ahead-1', 'Most ahead: ');
        const suffix = readLabelVar('--label-ahead-n', 'most ahead: ');
        return `${rank}${ordinalSuffix(rank)} ${suffix}`;
    }
    if (card.kind === 'behind')      return readLabelVar('--label-behind', 'Falling behind: ');
    if (card.kind === 'idle')        return readLabelVar('--label-idle', 'Most idle: ');
    if (card.kind === 'not_started') return readLabelVar('--label-not-started', 'Not started: ');
    return '';
}

// English ordinal suffix: 1→"st", 2→"nd", 3→"rd", 4..20→"th",
// then the cycle continues (21st, 22nd, 23rd, 24th... 111th, 112th, 113th).
function ordinalSuffix(n) {
    const tens = n % 100;
    if (tens >= 11 && tens <= 13) return 'th';
    const ones = n % 10;
    if (ones === 1) return 'st';
    if (ones === 2) return 'nd';
    if (ones === 3) return 'rd';
    return 'th';
}

// Mirror of OverviewWindow.cpp's idle-time formatter: up to two units,
// "now" / "Ns" / "NmNs" / "NhNm" / "NdNh" depending on magnitude.
function formatIdleDelta(seconds) {
    const d = Math.max(0, seconds);
    if (d < 1) return 'now';
    const i = Math.floor(d);
    if (i < 60)    return `${i}s`;
    if (i < 3600)  return `${Math.floor(i / 60)}m${i % 60}s`;
    if (i < 86400) return `${Math.floor(i / 3600)}h${Math.floor((i % 3600) / 60)}m`;
    return `${Math.floor(i / 86400)}d${Math.floor((i % 86400) / 3600)}h`;
}

// ---- Snapshot dispatch -------------------------------------------------

function onSnapshot(snap) {
    syncVisibilityClasses();
    // Apply ?players= override to flag is_watched.
    if (playersOverride.length > 0) {
        const wanted = new Set(playersOverride);
        for (const slot of snap.slots) {
            slot.is_watched = wanted.has(slot.name);
        }
    } else {
        for (const slot of snap.slots) slot.is_watched = false;
    }
    renderMySlots(snap);
    renderNotable(snap);
}

// Helpers for picking the "my-slots" set.
function pickMySlots(snap) {
    if (playersOverride.length > 0) {
        return snap.slots.filter(s => s.is_watched);
    }
    return snap.slots.filter(s => s.is_mine);
}

// ---- #my-slots ---------------------------------------------------------

const mineEl = document.getElementById('my-slots');

let mineCycleIdx = 0;
let mineCycleTimer = null;

function renderMySlots(snap) {
    const mine = pickMySlots(snap);
    if (mineMode === 'all' || mine.length <= 1) {
        // Stack everything; clear any rotation timer.
        clearTimeout(mineCycleTimer);
        mineCycleTimer = null;
        mineEl.innerHTML = '';
        for (const slot of mine) mineEl.appendChild(buildSlotRow(slot));
        return;
    }

    // We're switching into single-slot rendering. The "stacked" path
    // above appends rows without `.rotation-item`, so any leftover from
    // a prior render (e.g. when the user only had one connected slot
    // and we just now grew to two) would otherwise sit beside the
    // rotating row. Wipe non-rotation children before handing off.
    if (mineEl.querySelector(':scope > :not(.rotation-item)')) {
        mineEl.innerHTML = '';
    }

    if (mineMode === 'latest') {
        // Pick whichever has the newest last_activity > 0; fall back to first.
        let pick = mine[0];
        for (const slot of mine) {
            if (slot.last_activity > (pick.last_activity || 0)) pick = slot;
        }
        renderMineSingle(pick);
        return;
    }

    // 'cycle' mode — rotate through mine[] on a timer.
    if (mineCycleIdx >= mine.length) mineCycleIdx = 0;
    renderMineSingle(mine[mineCycleIdx]);
    if (!mineCycleTimer) {
        scheduleMineCycle(mine);
    }
}

function scheduleMineCycle(mine) {
    const seconds = readNumberVar('--stats-mine-cycle-seconds', 5);
    mineCycleTimer = setTimeout(() => {
        mineCycleTimer = null;
        if (!lastSnapshot) return;
        const fresh = pickMySlots(lastSnapshot);
        if (fresh.length <= 1) {
            renderMySlots(lastSnapshot);
            return;
        }
        mineCycleIdx = (mineCycleIdx + 1) % fresh.length;
        renderMineSingle(fresh[mineCycleIdx]);
        scheduleMineCycle(fresh);
    }, seconds * 1000);
}

function renderMineSingle(slot) {
    const key = `slot:${slot.slot}`;
    const existing = mineEl.querySelector(':scope > .rotation-item:not(.leaving)');
    const incoming = buildSlotRow(slot);
    incoming.classList.add('rotation-item');
    incoming.dataset.rotationKey = key;
    if (existing && existing.dataset.rotationKey === key) {
        // Same slot — refresh content in place so a WS update doesn't
        // retrigger the rotation animation.
        existing.className = incoming.className;
        existing.replaceChildren(...incoming.children);
        return;
    }
    swapRotationItem(mineEl, incoming);
}

// ---- #notable ----------------------------------------------------------

const notableEl = document.getElementById('notable');

let notableCycleIdx = 0;
let notableCycleTimer = null;

function computeNotable(snap) {
    const cards = [];
    const active = snap.slots.filter(s => !s.completed && s.total > 0);

    if (eligibleNotable.has('ahead')) {
        // Exclude 100% — a player at all checks done isn't "ahead", they're done.
        const candidates = active.filter(s => s.checked > 0 && s.checked < s.total);
        // Sort by ratio descending; tiebreak by slot id for stability.
        candidates.sort((a, b) => {
            const ra = a.checked / a.total;
            const rb = b.checked / b.total;
            if (rb !== ra) return rb - ra;
            return a.slot - b.slot;
        });
        const limit = Math.min(showTopAhead, candidates.length);
        for (let i = 0; i < limit; i++) {
            cards.push({ kind: 'ahead', slot: candidates[i], rank: i + 1 });
        }
    }
    if (eligibleNotable.has('behind')) {
        const withProgress = active.filter(s => s.checked > 0);
        if (withProgress.length > 0) {
            const loser = withProgress.reduce((a, b) =>
                (a.checked / a.total) <= (b.checked / b.total) ? a : b);
            // Don't duplicate someone we're already showing as an ahead card.
            const aheadSlots = new Set(cards
                .filter(c => c.kind === 'ahead')
                .map(c => c.slot.slot));
            if (!aheadSlots.has(loser.slot)) {
                cards.push({ kind: 'behind', slot: loser });
            }
        }
    }
    if (eligibleNotable.has('idle')) {
        const withActivity = active.filter(s => s.last_activity > 0);
        if (withActivity.length > 0) {
            const oldest = withActivity.reduce((a, b) =>
                a.last_activity <= b.last_activity ? a : b);
            const threshold = readNumberVar('--stats-idle-threshold-seconds', 600);
            if ((snap.now - oldest.last_activity) > threshold) {
                cards.push({ kind: 'idle', slot: oldest });
            }
        }
    }
    if (eligibleNotable.has('not_started')) {
        const count = snap.slots.filter(s =>
            !s.completed && s.total > 0 && s.checked === 0).length;
        if (count > 0) {
            cards.push({ kind: 'not_started', count });
        }
    }
    return cards;
}

function renderNotable(snap) {
    const cards = computeNotable(snap);
    if (cards.length === 0) {
        clearTimeout(notableCycleTimer);
        notableCycleTimer = null;
        notableEl.innerHTML = '';
        return;
    }
    if (notableCycleIdx >= cards.length) notableCycleIdx = 0;
    renderNotableSingle(cards[notableCycleIdx]);
    if (!notableCycleTimer && cards.length > 1) {
        scheduleNotableCycle();
    } else if (cards.length <= 1) {
        clearTimeout(notableCycleTimer);
        notableCycleTimer = null;
    }
}

function scheduleNotableCycle() {
    const seconds = readNumberVar('--stats-notable-cycle-seconds', 7);
    notableCycleTimer = setTimeout(() => {
        notableCycleTimer = null;
        if (!lastSnapshot) return;
        const cards = computeNotable(lastSnapshot);
        if (cards.length === 0) {
            notableEl.innerHTML = '';
            return;
        }
        notableCycleIdx = (notableCycleIdx + 1) % cards.length;
        renderNotableSingle(cards[notableCycleIdx]);
        if (cards.length > 1) scheduleNotableCycle();
    }, seconds * 1000);
}

function buildNotableCard(card) {
    const el = document.createElement('div');
    el.className = `card rotation-item ${card.kind.replace('_', '-')}`;
    if (card.kind === 'ahead') {
        const rank = card.rank || 1;
        el.classList.add(`rank-${rank}`);
        // data-rank holds the bare number for advanced CSS targeting.
        el.dataset.rank = String(rank);
    }
    const label = document.createElement('div');
    label.className = 'card-label';
    label.textContent = computeCardLabel(card);
    el.appendChild(label);
    const body = document.createElement('div');
    body.className = 'card-body';

    if (card.kind === 'not_started') {
        body.textContent = card.count === 1
            ? '1 world hasn’t started yet'
            : `${card.count} worlds haven’t started yet`;
    } else if (card.kind === 'ahead' || card.kind === 'behind') {
        // SlotName: [ ============ xx.x% (xxx/yyyyy) ============ ]
        // Game name
        const progressLine = document.createElement('div');
        progressLine.className = 'card-progress-line';

        const nameSpan = document.createElement('span');
        nameSpan.className = 'card-slot-name';
        nameSpan.textContent = `${card.slot.name}:`;
        progressLine.appendChild(nameSpan);
        progressLine.appendChild(buildProgressBar(card.slot));
        body.appendChild(progressLine);

        if (card.slot.game) {
            const gameLine = document.createElement('div');
            gameLine.className = 'card-game-line';
            gameLine.textContent = card.slot.game;
            body.appendChild(gameLine);
        }
    } else {
        // idle (and any other future single-slot kind): keep the inline
        // "Name (Game) detail" layout.
        body.textContent = card.slot.name;
        if (card.slot.game) {
            const detail = document.createElement('span');
            detail.className = 'card-detail';
            detail.textContent = `(${card.slot.game})`;
            body.appendChild(detail);
        }
        if (card.kind === 'idle') {
            // Show the elapsed silence instead of progress — that's what
            // makes "most idle" meaningful.
            const detail = document.createElement('span');
            detail.className = 'card-detail idle-time';
            detail.dataset.lastActivity = String(card.slot.last_activity);
            detail.textContent = formatIdleDelta(
                Date.now() / 1000 - card.slot.last_activity);
            body.appendChild(detail);
        }
    }
    el.appendChild(body);
    return el;
}

function renderNotableSingle(card) {
    const key = (card.kind === 'not_started')
        ? 'not_started'
        : (card.kind === 'ahead'
            ? `ahead:${card.rank}:${card.slot.slot}`
            : `${card.kind}:${card.slot.slot}`);
    const existing = notableEl.querySelector(':scope > .rotation-item:not(.leaving)');
    const fresh = buildNotableCard(card);
    fresh.dataset.rotationKey = key;
    if (existing && existing.dataset.rotationKey === key) {
        // Same logical card — refresh contents in place. Avoids replaying
        // the rotation animation every time a WS snapshot arrives.
        existing.className = fresh.className;
        existing.replaceChildren(...fresh.children);
        return;
    }
    swapRotationItem(notableEl, fresh);
}

// Tick the displayed idle delta every second. Self-prunes when the
// element has been removed from the DOM (e.g. rotated off).
setInterval(() => {
    const nodes = document.querySelectorAll('.idle-time');
    const now = Date.now() / 1000;
    for (const n of nodes) {
        const ts = parseFloat(n.dataset.lastActivity);
        if (isFinite(ts) && ts > 0) {
            n.textContent = formatIdleDelta(now - ts);
        }
    }
}, 1000);

// ---- Rotation transition machinery -------------------------------------

function swapRotationItem(container, incoming) {
    const transitionMs = readNumberVar(
        '--stats-rotation-transition-seconds', 0.4) * 1000;

    const outgoing = container.querySelector(':scope > .rotation-item:not(.leaving)');
    container.appendChild(incoming);
    incoming.classList.add('entering');

    if (outgoing && outgoing !== incoming) {
        outgoing.classList.add('leaving');
    }

    setTimeout(() => {
        incoming.classList.remove('entering');
        if (outgoing && outgoing.parentNode === container) {
            outgoing.remove();
        }
    }, transitionMs);
}

// ---- Progress bar primitive --------------------------------------------

// Builds the same .slot-bar-container / .slot-bar-track / .slot-bar-fill
// /.slot-bar-text structure used in #my-slots — reused by ahead/behind
// notable cards so they get identical bar styling.
function buildProgressBar(slot) {
    const barContainer = document.createElement('div');
    barContainer.className = 'slot-bar-container';

    const track = document.createElement('div');
    track.className = 'slot-bar-track';
    const fill = document.createElement('div');
    fill.className = 'slot-bar-fill';
    if (slot.total > 0) {
        const ratio = Math.max(0, Math.min(1, slot.checked / slot.total));
        const pct = ratio * 100;
        fill.style.width = `${pct.toFixed(2)}%`;
        // Drives the HSL hue ramp via `var(--pct)` in stats.css.
        fill.style.setProperty('--pct', pct.toFixed(2));
    }
    track.appendChild(fill);
    barContainer.appendChild(track);

    if (slot.total > 0) {
        const overlay = document.createElement('div');
        overlay.className = 'slot-bar-text';

        const pctEl = document.createElement('span');
        pctEl.className = 'slot-percent';
        pctEl.textContent = `${(slot.checked / slot.total * 100).toFixed(1)}%`;
        overlay.appendChild(pctEl);

        // Granular structure mirrors /overview so users can target/restyle
        // any piece (parens, slash, raw numbers) via Custom CSS.
        const counts = document.createElement('span');
        counts.className = 'slot-counts';
        const checkedEl = document.createElement('span');
        checkedEl.className = 'slot-checked';
        checkedEl.textContent = slot.checked;
        const totalEl = document.createElement('span');
        totalEl.className = 'slot-total';
        totalEl.textContent = slot.total;
        counts.append('(', checkedEl, '/', totalEl, ')');
        overlay.appendChild(counts);

        barContainer.appendChild(overlay);
    }

    return barContainer;
}

// ---- Slot row builder --------------------------------------------------

function buildSlotRow(slot) {
    const row = document.createElement('div');
    row.className = 'slot-row';
    if (slot.is_mine)     row.classList.add('is-mine');
    if (slot.is_watched)  row.classList.add('is-watched');
    if (slot.completed)   row.classList.add('completed');

    const header = document.createElement('div');
    header.className = 'slot-header';

    const name = document.createElement('span');
    name.className = 'slot-name';
    name.textContent = slot.name;
    header.appendChild(name);

    if (slot.game) {
        const game = document.createElement('span');
        game.className = 'slot-game';
        game.textContent = slot.game;
        header.appendChild(game);
    }

    row.appendChild(header);
    row.appendChild(buildProgressBar(slot));
    return row;
}

// ---- Goal popup --------------------------------------------------------

const goalEl = document.getElementById('goal-popup');
const goalNameEl = goalEl.querySelector('.goal-name');
const goalGameEl = goalEl.querySelector('.goal-game');
const goalGamePrefix = goalEl.querySelector('.goal-game-prefix');

const goalQueue = [];
let goalShowing = false;

function onGoalEvent(ev) {
    goalQueue.push(ev);
    if (!goalShowing) showNextGoal();
}

// Read the burst palette from the --stats-burst-palette CSS variable
// (a quoted, comma-separated string). Used so each particle can pick
// its own bright color rather than a wall of monochrome.
function getBurstPalette() {
    const raw = readLabelVar('--stats-burst-palette',
        '#ff3b3b, #ff9d00, #ffeb3b, #4caf50, #2196f3, #ab47bc');
    const colors = raw.split(',').map(s => s.trim()).filter(s => s.length > 0);
    return colors.length > 0 ? colors : ['#ffffff'];
}

function pickColor(palette) {
    return palette[Math.floor(Math.random() * palette.length)];
}

// Generate the shooting sparks and twinkling sparkles for one popup
// firing. Each particle gets random position / direction / delay /
// size / color; CSS animations handle the actual motion and fading.
function spawnGoalParticles() {
    const fw = goalEl.querySelector('.fireworks');
    const sp = goalEl.querySelector('.sparkles');
    fw.replaceChildren();
    sp.replaceChildren();
    const palette = getBurstPalette();

    // Two waves of shooting sparks — initial big burst, then a smaller
    // secondary burst so the popup doesn't go quiet after the first half-
    // second.
    const waves = [
        { count: 32, baseDelay: 0,    minDist: 160, maxDist: 360 },
        { count: 18, baseDelay: 1.6,  minDist: 100, maxDist: 240 },
    ];
    for (const w of waves) {
        // Pick one base color for the wave so its sparks read as a
        // single coordinated burst, with a few sparks rerolled to other
        // palette colors for variety.
        const baseColor = pickColor(palette);
        for (let i = 0; i < w.count; i++) {
            const angle = (i / w.count) * Math.PI * 2 +
                          (Math.random() - 0.5) * 0.5;
            const dist = w.minDist + Math.random() * (w.maxDist - w.minDist);
            const el = document.createElement('span');
            el.className = 'spark';
            el.style.color =
                Math.random() < 0.25 ? pickColor(palette) : baseColor;
            el.style.setProperty('--dx', `${(Math.cos(angle) * dist).toFixed(1)}px`);
            el.style.setProperty('--dy', `${(Math.sin(angle) * dist).toFixed(1)}px`);
            el.style.setProperty('--delay',
                `${(w.baseDelay + Math.random() * 0.18).toFixed(3)}s`);
            el.style.setProperty('--size', `${(4 + Math.random() * 4).toFixed(1)}px`);
            fw.appendChild(el);
        }
    }

    // Mini explosions scattered across the popup, popping in over the
    // popup's lifetime so the action doesn't all happen at the center.
    const miniCount = 12;
    for (let i = 0; i < miniCount; i++) {
        const el = document.createElement('span');
        el.className = 'mini-burst';
        el.style.color = pickColor(palette);
        // Stay slightly inside the edges so the burst is fully visible.
        el.style.setProperty('--x', `${(15 + Math.random() * 70).toFixed(1)}%`);
        el.style.setProperty('--y', `${(15 + Math.random() * 70).toFixed(1)}%`);
        el.style.setProperty('--delay',
            `${(0.35 + Math.random() * 3.0).toFixed(3)}s`);
        el.style.setProperty('--size',
            `${(40 + Math.random() * 70).toFixed(1)}px`);
        fw.appendChild(el);
    }

    // Twinkling sparkles scattered across the popup, each on its own
    // staggered loop.
    const sparkleCount = 28;
    for (let i = 0; i < sparkleCount; i++) {
        const el = document.createElement('span');
        el.className = 'sparkle';
        el.style.color = pickColor(palette);
        el.style.setProperty('--x', `${(Math.random() * 100).toFixed(1)}%`);
        el.style.setProperty('--y', `${(Math.random() * 100).toFixed(1)}%`);
        el.style.setProperty('--delay', `${(Math.random() * 1.4).toFixed(3)}s`);
        el.style.setProperty('--size', `${(2 + Math.random() * 3).toFixed(1)}px`);
        sp.appendChild(el);
    }
}

function showNextGoal() {
    const ev = goalQueue.shift();
    if (!ev) {
        goalShowing = false;
        return;
    }
    goalShowing = true;
    goalNameEl.textContent = ev.name || 'Someone';
    if (ev.game) {
        goalGameEl.textContent = ev.game;
        goalGameEl.style.display = '';
        goalGamePrefix.style.display = '';
    } else {
        goalGameEl.style.display = 'none';
        goalGamePrefix.style.display = 'none';
    }

    spawnGoalParticles();

    // Force reflow so the animation restarts on consecutive popups.
    goalEl.classList.remove('visible');
    void goalEl.offsetWidth;
    goalEl.classList.add('visible');

    const durationMs = (() => {
        // Read --stats-goal-duration which may include a unit suffix (s/ms).
        const raw = getComputedStyle(document.body)
            .getPropertyValue('--stats-goal-duration').trim();
        if (raw.endsWith('ms')) return parseFloat(raw);
        if (raw.endsWith('s'))  return parseFloat(raw) * 1000;
        return parseFloat(raw) || 5000;
    })();

    setTimeout(() => {
        goalEl.classList.remove('visible');
        // Brief gap before the next popup, if queued.
        setTimeout(showNextGoal, 250);
    }, durationMs);
}

// ---- Boot --------------------------------------------------------------

connect();

})();
