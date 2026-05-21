async function requestJson(url, options) {
  const response = await fetch(url, options);
  const text = await response.text();
  let body = {};
  try {
    body = text ? JSON.parse(text) : {};
  } catch (_) {
    body = { raw: text };
  }
  if (!response.ok) {
    throw new Error((body && body.error) || ("HTTP " + response.status));
  }
  return body;
}

let refreshInFlight = false;
let lastTaskSignature = "";
let detailTaskId = 0;
let detailRefreshTimer = 0;
let detailInFlight = false;
let eventsSource = null;
let usingSse = false;
let fallbackPollTimer = 0;

const settingsElementIds = [
  "maxPeers", "listenPort", "listenPortEnd", "downloadLimit", "uploadLimit", "maxConcurrent",
  "enableDht", "enablePex", "enableUpnp"
];

function byId(id) {
  return document.getElementById(id);
}

function escHtml(value) {
  return String(value == null ? "" : value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function kvRows(obj) {
  return Object.entries(obj)
    .map(([k, v]) => `<div class="kv"><span class="k">${escHtml(k)}</span><span class="v">${escHtml(v)}</span></div>`)
    .join("");
}

function setMessage(message, ok) {
  const el = byId("controlMsg");
  if (!el) return;
  el.innerHTML = ok ? `<span class="ok">${escHtml(message)}</span>` : `<span class="err">${escHtml(message)}</span>`;
}

function setSyncState(ok, detail) {
  const el = byId("syncState");
  if (!el) return;
  const now = new Date();
  el.className = "sync-state " + (ok ? "sync-ok" : "sync-error");
  el.textContent = ok ? "Online - " + now.toLocaleTimeString() : "Offline - " + (detail || "sync failed");
}

function isEditingSettings() {
  const active = document.activeElement;
  return !!active && settingsElementIds.indexOf(active.id) >= 0;
}

function setInputValueIfIdle(id, value) {
  const el = byId(id);
  if (!el || document.activeElement === el) return;
  el.value = value == null ? "" : value;
}

function setCheckboxIfIdle(id, checked) {
  const el = byId(id);
  if (!el || document.activeElement === el) return;
  el.checked = checked;
}

function formatBytes(bytes) {
  bytes = Number(bytes) || 0;
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + " KB";
  if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + " MB";
  return (bytes / 1073741824).toFixed(2) + " GB";
}

function formatDuration(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "-";
  if (seconds < 60) return Math.round(seconds) + " sec";
  if (seconds < 3600) return Math.round(seconds / 60) + " min";
  return (seconds / 3600).toFixed(1) + " hr";
}

function formatCountdown(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "-";
  if (seconds <= 0) return "now";
  return formatDuration(seconds);
}

function formatStatus(status) {
  const map = {
    queued: "Queued",
    loaded: "Loaded",
    running: "Running",
    paused: "Paused",
    seeding: "Seeding",
    error: "Error",
    stopped: "Stopped",
    completed: "Completed"
  };
  return map[status] || status || "Unknown";
}

function formatError(error) {
  const map = {
    load_torrent_failed: "Failed to load torrent",
    load_magnet_failed: "Failed to load magnet URI",
    start_task_failed: "Failed to start task",
    task_not_active: "Task is not active",
    task_completed: "Task is completed",
    add_task_failed: "Failed to add task",
    invalid_torrent_data: "Invalid torrent data",
    save_torrent_failed: "Failed to save torrent file",
    torrent_data_empty: "Torrent data is empty",
    add_magnet_task_failed: "Failed to add magnet task",
    max_concurrent_reached: "Max concurrent downloads reached"
  };
  return map[error] || error || "";
}

function taskSignature(tasks) {
  return JSON.stringify((tasks || []).map(function(t) {
    return [t.id, t.status, t.running, t.progress, t.download_speed, t.peer_count, t.last_error, t.updated_at_ms];
  }));
}

function updateSettings(data) {
  if (isEditingSettings() || !data.bt) return;
  setInputValueIfIdle("maxPeers", data.bt.max_peers || "");
  setInputValueIfIdle("listenPort", data.bt.listen_port || "");
  setInputValueIfIdle("listenPortEnd", data.bt.listen_port_end || "");
  setInputValueIfIdle("downloadLimit", data.bt.download_limit_kbps || "");
  setInputValueIfIdle("uploadLimit", data.bt.upload_limit_kbps || "");
  setInputValueIfIdle("maxConcurrent", data.bt.max_concurrent || 3);
  setCheckboxIfIdle("enableDht", data.bt.enable_dht !== false);
  setCheckboxIfIdle("enablePex", data.bt.enable_pex !== false);
  setCheckboxIfIdle("enableUpnp", data.bt.enable_upnp !== false);
}

function handleOverviewData(data) {
  setSyncState(true, usingSse ? "SSE" : "poll");

  byId("overview").innerHTML = kvRows({
    app: data.app_name || "-",
    worker: data.worker_index,
    services: data.service_count,
    last_event_ms: data.last_event_ms || 0
  });

  const bt = data.bt || {};
  byId("bt").innerHTML = kvRows({
    running: bt.running ? "yes" : "no",
    active_tasks: (bt.active_count || 0) + "/" + (bt.max_concurrent || 3),
    metadata_mode: bt.metadata_mode ? "fetching metadata" : "no",
    complete: bt.complete ? "yes" : "no",
    peers: bt.peer_count || 0,
    max_peers: bt.max_peers || 0,
    listen_port: (bt.listen_port || 6881) + "-" + (bt.listen_port_end || 6999),
    download_limit: (bt.download_limit_kbps || 0) + " KB/s",
    upload_limit: (bt.upload_limit_kbps || 0) + " KB/s",
    downloaded: formatBytes(bt.downloaded_bytes || 0),
    uploaded: formatBytes(bt.uploaded_bytes || 0),
    ratio: Number(bt.ratio || 0).toFixed(2),
    progress: ((bt.progress || 0) * 100).toFixed(1) + "%",
    pieces: (bt.pieces_downloaded || 0) + " / " + (bt.pieces_total || 0),
    speed_now: formatBytes(bt.download_speed || 0) + "/s",
    upload_now: formatBytes(bt.upload_speed || 0) + "/s",
    dht: bt.dht_running ? ("running, nodes=" + (bt.dht_nodes || 0)) : "off",
    nat: bt.nat_mapped
      ? ("mapped " + (bt.nat_external_ip || "?") + ":" + (bt.nat_mapped_port || 0))
      : (bt.nat_igd_discovered ? "IGD found, not mapped" : "not mapped"),
    last_info_hash: bt.last_info_hash || "-",
    last_torrent: bt.last_torrent_name || "-"
  });

  byId("raw").textContent = JSON.stringify({
    service_states: data.service_states,
    event_counters: data.event_counters,
    recent_events: data.recent_events
  }, null, 2);

  byId("taskHistory").innerHTML = renderHistory(data.bt_task_history || []);

  const tasks = bt.tasks || [];
  const errorTasks = tasks.filter(t => t.status === "error");
  const errorBanner = byId("errorBanner");
  if (errorTasks.length > 0) {
    let html = `<strong>${errorTasks.length} task(s) failed</strong><ul>`;
    for (const t of errorTasks) {
      html += `<li>#${t.id} ${escHtml(t.name || t.info_hash || "unknown")} - ${escHtml(formatError(t.last_error))} <button class="retry" onclick="retryTaskByIdValue(${t.id})">Retry</button></li>`;
    }
    html += "</ul>";
    errorBanner.innerHTML = html;
    errorBanner.style.display = "block";
  } else {
    errorBanner.style.display = "none";
  }

  byId("taskSummary").innerHTML = renderTaskSummary(tasks, bt.active_count || 0, bt.max_concurrent || 3);
  const sig = taskSignature(tasks);
  if (sig !== lastTaskSignature) {
    byId("taskList").innerHTML = renderTaskTable(tasks, bt.active_count || 0, bt.max_concurrent || 3);
    lastTaskSignature = sig;
  }

  updateSettings(data);
}

function renderTaskSummary(tasks, activeCount, maxConcurrent) {
  const counts = { queued: 0, running: 0, paused: 0, seeding: 0, completed: 0, stopped: 0, error: 0 };
  let totalSpeed = 0;
  let totalUploadSpeed = 0;
  let totalRemaining = 0;
  for (const t of tasks || []) {
    const status = t.status || (t.running ? "running" : "stopped");
    counts[status] = (counts[status] || 0) + 1;
    totalSpeed += t.download_speed || 0;
    totalUploadSpeed += t.upload_speed || 0;
    if ((t.total_bytes || 0) > 0) totalRemaining += Math.max(0, (t.total_bytes || 0) - (t.downloaded_bytes || 0));
  }
  const eta = totalSpeed > 0 && totalRemaining > 0 ? formatDuration(totalRemaining / totalSpeed) : "-";
  return `<span class="summary-pill">Running ${activeCount}/${maxConcurrent}</span>`
    + `<span class="summary-pill">Queued ${counts.queued || 0}</span>`
    + `<span class="summary-pill">Paused ${counts.paused || 0}</span>`
    + `<span class="summary-pill">Seeding ${counts.seeding || 0}</span>`
    + `<span class="summary-pill">Failed ${counts.error || 0}</span>`
    + `<span class="summary-pill">Down ${formatBytes(totalSpeed)}/s</span>`
    + `<span class="summary-pill">Up ${formatBytes(totalUploadSpeed)}/s</span>`
    + `<span class="summary-pill">ETA ${eta}</span>`;
}

function renderTaskTable(tasks, activeCount, maxConcurrent) {
  if (!tasks || !tasks.length) {
    return '<div class="empty-hint">No tasks yet</div>';
  }

  let html = '<table class="task-table"><thead><tr>'
    + '<th>ID</th><th>Name</th><th>Size</th><th>Progress</th><th>Speed</th><th>ETA</th><th>Status</th><th>Peers</th><th>Type</th><th>Actions</th>'
    + '</tr></thead><tbody>';

  for (const t of tasks) {
    const isActive = t.active || t.running;
    const progressPct = t.progress != null ? Math.round(t.progress * 100) : 0;
    const progressText = t.progress != null && t.progress > 0 ? (t.progress * 100).toFixed(1) + "%" : "-";
    const size = t.total_bytes > 0 ? formatBytes(t.total_bytes) : "-";
    const remaining = Math.max(0, (t.total_bytes || 0) - (t.downloaded_bytes || 0));
    const eta = t.download_speed > 0 && remaining > 0 ? formatDuration(remaining / t.download_speed) : "-";
    const speed = t.status === "seeding" && t.uploaded_bytes > 0
      ? "up " + formatBytes(t.upload_speed || 0) + "/s"
      : t.download_speed > 0 ? "down " + formatBytes(t.download_speed) + "/s" : "-";
    const statusClass = t.status === "error" ? "status-error"
      : t.status === "seeding" ? "status-seeding"
      : t.status === "completed" ? "status-completed"
      : t.running ? "status-running" : "status-stopped";
    const statusCell = t.status === "error" && t.last_error
      ? `<span class="${statusClass}">${formatStatus(t.status)}</span><div class="error-detail">${escHtml(formatError(t.last_error))}</div>`
      : `<span class="${statusClass}">${formatStatus(t.status || (t.running ? "running" : "stopped"))}</span>`;
    const startBtn = (!t.running && t.status !== "error" && t.status !== "seeding" && t.status !== "paused") ? `<button onclick="startTaskByIdValue(${t.id})">Start</button>` : "";
    const resumeBtn = t.status === "paused" ? `<button onclick="resumeTaskByIdValue(${t.id})">Resume</button>` : "";
    const retryBtn = t.status === "error" ? `<button class="retry" onclick="retryTaskByIdValue(${t.id})">Retry</button>` : "";
    const pauseBtn = t.running ? `<button onclick="pauseTaskByIdValue(${t.id})">Pause</button>` : "";
    const stopBtn = (t.running || t.status === "seeding") ? `<button onclick="stopTaskByIdValue(${t.id})">Stop</button>` : "";
    const type = t.magnet_uri ? "Magnet" : (t.has_torrent_data ? "Upload" : "File");

    html += `<tr class="${isActive ? "row-active" : ""}">`
      + `<td>${t.id}</td>`
      + `<td class="cell-name" title="${escHtml(t.name || t.info_hash || "-")}">${escHtml(t.name || t.info_hash || "-")}</td>`
      + `<td>${size}</td>`
      + `<td><div class="progress-cell"><div class="progress-bar" style="width:${progressPct}%"></div><span class="progress-text">${progressText}</span></div></td>`
      + `<td>${speed}<div class="subtle">R ${Number(t.ratio || 0).toFixed(2)}</div></td>`
      + `<td>${eta}</td>`
      + `<td>${statusCell}</td>`
      + `<td>${t.peer_count != null ? t.peer_count : "-"}</td>`
      + `<td>${type}</td>`
      + `<td class="cell-actions"><button onclick="showTaskDetail(${t.id})">Detail</button>${retryBtn}${resumeBtn}${startBtn}${pauseBtn}${stopBtn}<button class="warn" onclick="removeTaskByIdValue(${t.id})">Remove</button></td>`
      + "</tr>";
  }
  html += "</tbody></table>";
  return html;
}

function renderHistory(history) {
  if (!history || !history.length) {
    return '<div class="empty-hint">No history yet</div>';
  }
  let html = '<table class="detail-table"><thead><tr><th>Time</th><th>Action</th><th>Name</th><th>Save Path</th><th>Status</th></tr></thead><tbody>';
  for (const item of history) {
    const ts = item.ts ? new Date(item.ts).toLocaleString() : "-";
    html += `<tr><td>${escHtml(ts)}</td><td>${escHtml(item.action || "-")}</td><td>${escHtml(item.name || item.info_hash || "-")}</td><td>${escHtml(item.save_path || "-")}</td><td>${item.running ? '<span class="status-running">Running</span>' : '<span class="status-stopped">Stopped</span>'}</td></tr>`;
  }
  html += "</tbody></table>";
  return html;
}

function renderTaskDetail(data) {
  let html = '<div class="detail-metrics">'
    + `<span>Status ${formatStatus(data.running ? "running" : (data.active ? "loaded" : "stopped"))}</span>`
    + `<span>Metadata ${data.metadata_mode ? "fetching" : "ready"}</span>`
    + `<span>Info Hash: ${escHtml(data.info_hash || "-")}</span>`
    + "</div>";

  html += '<div class="detail-section"><h4>Files</h4>';
  if (data.files && data.files.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>Path</th><th>Size</th><th>Offset</th></tr></thead><tbody>';
    for (const f of data.files) {
      html += `<tr><td>${escHtml(f.name || "-")}</td><td>${formatBytes(f.length || 0)}</td><td>${f.offset || 0}</td></tr>`;
    }
    html += "</tbody></table>";
  } else {
    html += '<div class="empty-hint">No file metadata yet</div>';
  }
  html += "</div>";

  html += '<div class="detail-section"><h4>Pieces</h4>';
  const p = data.pieces || {};
  if (p.total > 0) {
    const donePct = Math.round((p.done / p.total) * 100);
    const activePct = Math.round((p.active / p.total) * 100);
    const pendingPct = Math.max(0, 100 - donePct - activePct);
    html += '<div class="pieces-bar">';
    if (p.done > 0) html += `<div class="seg seg-done" style="width:${donePct}%">${p.done}</div>`;
    if (p.active > 0) html += `<div class="seg seg-active" style="width:${activePct}%">${p.active}</div>`;
    if (p.pending > 0) html += `<div class="seg seg-pending" style="width:${pendingPct}%">${p.pending}</div>`;
    html += `</div><div class="detail-note">Done ${p.done} / active ${p.active} / pending ${p.pending} / total ${p.total}${p.endgame ? " (endgame)" : ""}</div>`;
  } else {
    html += '<div class="empty-hint">No piece data yet</div>';
  }
  html += "</div>";

  html += `<div class="detail-section"><h4>Peers (${data.peers ? data.peers.length : 0})</h4>`;
  if (data.peers && data.peers.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>Address</th><th>State</th><th>Down</th><th>Up</th><th>Pieces</th><th>Flags</th></tr></thead><tbody>';
    for (const peer of data.peers) {
      const flags = [];
      if (peer.peer_choking) flags.push("peer choking");
      if (!peer.am_interested) flags.push("not interested");
      if (peer.snubbed) flags.push("snubbed");
      html += `<tr><td>${escHtml(peer.ip || "?")}:${peer.port || 0}</td><td><span class="tag tag-${escHtml(peer.state || "idle")}">${escHtml(peer.state || "?")}</span></td><td>${formatBytes(peer.download_rate || 0)}/s</td><td>${formatBytes(peer.upload_rate || 0)}/s</td><td>${peer.pieces_have || 0}/${peer.pieces_total || 0}</td><td>${flags.length ? escHtml(flags.join(", ")) : "-"}</td></tr>`;
    }
    html += "</tbody></table>";
  } else {
    html += '<div class="empty-hint">No connected peers</div>';
  }
  html += "</div>";

  html += '<div class="detail-section"><h4>Trackers</h4>';
  if (data.trackers && data.trackers.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>URL</th><th>Tier</th><th>Source</th><th>Last</th><th>Peers</th><th>Interval</th><th>Next</th></tr></thead><tbody>';
    for (const trk of data.trackers) {
      const last = trk.last_announce_ms ? new Date(trk.last_announce_ms).toLocaleTimeString() : "-";
      let state = trk.last_announce_ms
        ? (trk.last_success ? '<span class="status-running">Success</span>' : '<span class="status-error">Failed</span>')
        : '<span class="status-stopped">Not announced</span>';
      if (trk.error) state += `<div class="error-detail">${escHtml(formatError(trk.error))}</div>`;
      let next = "-";
      if (trk.last_announce_ms && trk.interval) {
        next = formatCountdown(trk.interval - ((Date.now() - trk.last_announce_ms) / 1000));
      }
      html += `<tr><td>${escHtml(trk.url || "-")}</td><td>${trk.tier != null ? trk.tier : "-"}</td><td>${escHtml(trk.source || "torrent")}</td><td>${state}<div class="subtle">${escHtml(last)}</div></td><td>${trk.peers || 0}</td><td>${trk.interval || 0}s</td><td>${next}</td></tr>`;
    }
    html += "</tbody></table>";
  } else {
    html += '<div class="empty-hint">No tracker data yet</div>';
  }
  html += "</div>";
  return html;
}

async function refreshTaskDetail() {
  if (!detailTaskId || detailInFlight) return;
  detailInFlight = true;
  const sync = byId("detailSync");
  try {
    const data = await requestJson("/admin/api/bt/tasks/detail?task_id=" + detailTaskId);
    byId("detailBody").innerHTML = renderTaskDetail(data);
    if (sync) sync.textContent = "Auto refresh - " + new Date().toLocaleTimeString();
  } catch (error) {
    byId("detailBody").innerHTML = `<div class="empty-hint err">Load failed: ${escHtml(error.message || String(error))}</div>`;
    if (sync) sync.textContent = "Refresh failed";
  } finally {
    detailInFlight = false;
  }
}

async function showTaskDetail(taskId) {
  const modal = byId("detailModal");
  detailTaskId = taskId;
  byId("detailTitle").textContent = "Task #" + taskId + " Detail";
  byId("detailBody").innerHTML = '<div class="empty-hint">Loading...</div>';
  modal.style.display = "flex";
  await refreshTaskDetail();
  clearInterval(detailRefreshTimer);
  detailRefreshTimer = setInterval(refreshTaskDetail, 1000);
}

function closeTaskDetail() {
  detailTaskId = 0;
  clearInterval(detailRefreshTimer);
  detailRefreshTimer = 0;
  byId("detailModal").style.display = "none";
}

async function refresh() {
  if (refreshInFlight) return;
  refreshInFlight = true;
  try {
    const data = await requestJson("/admin/api/overview");
    handleOverviewData(data);
  } catch (error) {
    setSyncState(false, error.message || String(error));
    setMessage(error.message || String(error), false);
  } finally {
    refreshInFlight = false;
  }
}

async function refreshTasks() {
  try {
    const data = await requestJson("/admin/api/bt/tasks");
    const tasks = data.items || [];
    byId("taskSummary").innerHTML = renderTaskSummary(tasks, data.active_count || 0, data.max_concurrent || 3);
    byId("taskList").innerHTML = renderTaskTable(tasks, data.active_count || 0, data.max_concurrent || 3);
    lastTaskSignature = taskSignature(tasks);
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function postTaskAction(path, id, successMessage) {
  const data = await requestJson(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ task_id: id })
  });
  setMessage(data.message || successMessage, true);
  await refresh();
}

async function startTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/start", id, "task started");
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function retryTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/start", id, "task retried");
  } catch (error) {
    setMessage("Retry failed: " + formatError(error.message || String(error)), false);
  }
}

async function stopTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/stop", id, "task stopped");
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function pauseTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/pause", id, "task paused");
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function resumeTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/resume", id, "task resumed");
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function removeTaskByIdValue(id) {
  try {
    await postTaskAction("/admin/api/bt/tasks/remove", id, "task removed");
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function startTaskById() {
  const id = Number(byId("taskStartId").value || 0);
  if (!id) return setMessage("Enter a valid task ID", false);
  await startTaskByIdValue(id);
}

async function stopTaskById() {
  const id = Number(byId("taskStopId").value || 0);
  if (!id) return setMessage("Enter a valid task ID", false);
  await stopTaskByIdValue(id);
}

async function removeTaskById() {
  const id = Number(byId("taskRemoveId").value || 0);
  if (!id) return setMessage("Enter a valid task ID", false);
  await removeTaskByIdValue(id);
}

async function control(action) {
  setMessage("Processing...", true);
  try {
    const data = await requestJson("/admin/api/bt/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action })
    });
    setMessage(data.message || "ok", true);
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function loadTorrent() {
  const torrentPath = byId("torrentPath").value.trim();
  const savePath = byId("savePath").value.trim();
  if (!torrentPath) return setMessage("Enter a .torrent path", false);
  try {
    const data = await requestJson("/admin/api/bt/torrent", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ torrent_path: torrentPath, save_path: savePath })
    });
    setMessage(data.queued ? "Task queued because concurrency is full" : (data.message || "torrent loaded"), !data.queued);
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function uploadTorrent() {
  const fileInput = byId("torrentFile");
  const file = fileInput && fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
  if (!file) return setMessage("Choose a .torrent file first", false);

  const formData = new FormData();
  formData.append("torrent", file);
  const savePath = byId("savePath").value.trim();
  if (savePath) formData.append("save_path", savePath);

  try {
    const data = await requestJson("/admin/api/bt/upload", { method: "POST", body: formData });
    setMessage(data.queued ? "Task queued because concurrency is full" : (data.message || "torrent uploaded"), !data.queued);
    fileInput.value = "";
    byId("torrentFileName").textContent = "";
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function loadMagnet() {
  const magnetUri = byId("magnetUri").value.trim();
  const savePath = byId("savePath").value.trim();
  if (!magnetUri) return setMessage("Enter a magnet URI", false);
  try {
    const data = await requestJson("/admin/api/bt/magnet", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ magnet_uri: magnetUri, save_path: savePath })
    });
    setMessage(data.queued ? "Task queued because concurrency is full" : (data.message || "magnet added"), !data.queued);
    byId("magnetUri").value = "";
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function applySettings() {
  const payload = {};
  const numberFields = [
    ["maxPeers", "max_peers"],
    ["listenPort", "listen_port"],
    ["listenPortEnd", "listen_port_end"],
    ["downloadLimit", "download_limit_kbps"],
    ["uploadLimit", "upload_limit_kbps"],
    ["maxConcurrent", "max_concurrent_downloads"]
  ];
  for (const [id, key] of numberFields) {
    const raw = byId(id).value.trim();
    if (raw !== "") payload[key] = Number(raw);
  }
  payload.enable_dht = byId("enableDht").checked;
  payload.enable_pex = byId("enablePex").checked;
  payload.enable_upnp = byId("enableUpnp").checked;

  try {
    const data = await requestJson("/admin/api/bt/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    });
    setMessage(data.message || "settings updated", true);
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

function downloadTextFile(filename, content) {
  const blob = new Blob([content], { type: "application/json;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

async function exportConfigTemplate() {
  try {
    const data = await requestJson("/admin/api/overview");
    const bt = data.bt || {};
    const cfg = {
      app_name: data.app_name || "bt_downloader",
      admin_port: 18080,
      save_path: ".",
      enable_keep_alive: true,
      enable_cors: true,
      bt: {
        max_peers: bt.max_peers || 50,
        listen_port: bt.listen_port || 6881,
        listen_port_end: bt.listen_port_end || 6999,
        download_limit_kbps: bt.download_limit_kbps || 0,
        upload_limit_kbps: bt.upload_limit_kbps || 0,
        max_concurrent_downloads: bt.max_concurrent || 3,
        enable_dht: bt.enable_dht !== false,
        enable_pex: bt.enable_pex !== false,
        enable_upnp: bt.enable_upnp !== false
      }
    };
    downloadTextFile("bt_downloader.config.template.json", JSON.stringify(cfg, null, 2));
    setMessage("Config exported", true);
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function applyImportedConfig(config) {
  const settingsPayload = {};
  if (config && config.bt && typeof config.bt === "object") {
    if (Number.isFinite(Number(config.bt.max_peers))) settingsPayload.max_peers = Number(config.bt.max_peers);
    if (Number.isFinite(Number(config.bt.listen_port))) settingsPayload.listen_port = Number(config.bt.listen_port);
    if (Number.isFinite(Number(config.bt.listen_port_end))) settingsPayload.listen_port_end = Number(config.bt.listen_port_end);
    if (Number.isFinite(Number(config.bt.download_limit_kbps))) settingsPayload.download_limit_kbps = Number(config.bt.download_limit_kbps);
    if (Number.isFinite(Number(config.bt.upload_limit_kbps))) settingsPayload.upload_limit_kbps = Number(config.bt.upload_limit_kbps);
    if (Number.isFinite(Number(config.bt.max_concurrent_downloads))) settingsPayload.max_concurrent_downloads = Number(config.bt.max_concurrent_downloads);
    if (typeof config.bt.enable_dht === "boolean") settingsPayload.enable_dht = config.bt.enable_dht;
    if (typeof config.bt.enable_pex === "boolean") settingsPayload.enable_pex = config.bt.enable_pex;
    if (typeof config.bt.enable_upnp === "boolean") settingsPayload.enable_upnp = config.bt.enable_upnp;
  }
  if (Object.keys(settingsPayload).length > 0) {
    await requestJson("/admin/api/bt/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(settingsPayload)
    });
  }
  if (config && config.torrent_file && String(config.torrent_file).trim()) {
    await requestJson("/admin/api/bt/torrent", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        torrent_path: String(config.torrent_file).trim(),
        save_path: config.save_path ? String(config.save_path).trim() : ""
      })
    });
  }
}

async function importConfigTemplate() {
  const fileInput = byId("btImportConfigFile");
  const file = fileInput && fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
  if (!file) return setMessage("Choose a config file first", false);
  try {
    const config = JSON.parse(await file.text());
    await applyImportedConfig(config);
    fileInput.value = "";
    setMessage("Config imported", true);
    await refresh();
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

async function loadHistory() {
  const page = Number(byId("historyPage").value || 1);
  const pageSize = Number(byId("historyPageSize").value || 20);
  try {
    const data = await requestJson(`/admin/api/bt/history?page=${page}&page_size=${pageSize}`);
    byId("taskHistory").innerHTML = renderHistory(data.items || []);
  } catch (error) {
    setMessage(formatError(error.message || String(error)), false);
  }
}

function startFallbackPolling(intervalMs) {
  clearInterval(fallbackPollTimer);
  fallbackPollTimer = setInterval(refresh, intervalMs);
}

function connectEvents() {
  if (!window.EventSource) {
    startFallbackPolling(2000);
    refresh();
    return;
  }
  try {
    eventsSource = new EventSource("/admin/api/bt/events");
  } catch (_) {
    startFallbackPolling(2000);
    refresh();
    return;
  }

  eventsSource.addEventListener("ready", function() {
    usingSse = true;
    clearInterval(fallbackPollTimer);
    refresh();
  });

  eventsSource.addEventListener("overview", function(event) {
    usingSse = true;
    clearInterval(fallbackPollTimer);
    try {
      handleOverviewData(JSON.parse(event.data));
    } catch (_) {
      setSyncState(false, "bad SSE data");
    }
  });

  eventsSource.onerror = function() {
    usingSse = false;
    setSyncState(false, "SSE disconnected");
    startFallbackPolling(2000);
  };
}

function bindEvents() {
  byId("btStart").addEventListener("click", () => control("start"));
  byId("btStop").addEventListener("click", () => control("stop"));
  byId("btLoad").addEventListener("click", loadTorrent);
  byId("btUpload").addEventListener("click", uploadTorrent);
  byId("btMagnet").addEventListener("click", loadMagnet);
  byId("btApplySettings").addEventListener("click", applySettings);
  byId("btExportConfig").addEventListener("click", exportConfigTemplate);
  byId("btImportConfigApply").addEventListener("click", importConfigTemplate);
  byId("historyLoad").addEventListener("click", loadHistory);
  byId("taskRefreshBtn").addEventListener("click", refreshTasks);
  byId("taskStartBtn").addEventListener("click", startTaskById);
  byId("taskStopBtn").addEventListener("click", stopTaskById);
  byId("taskRemoveBtn").addEventListener("click", removeTaskById);
  byId("detailClose").addEventListener("click", closeTaskDetail);
  byId("detailModal").addEventListener("click", function(e) {
    if (e.target === this) closeTaskDetail();
  });
  byId("torrentFile").addEventListener("change", function() {
    byId("torrentFileName").textContent = this.files && this.files[0] ? this.files[0].name : "";
  });

  const zone = byId("dropZone");
  const fileInput = byId("torrentFile");
  zone.addEventListener("click", () => fileInput.click());
  zone.addEventListener("dragover", function(e) {
    e.preventDefault();
    zone.classList.add("drag-over");
  });
  zone.addEventListener("dragleave", function(e) {
    e.preventDefault();
    zone.classList.remove("drag-over");
  });
  zone.addEventListener("drop", function(e) {
    e.preventDefault();
    zone.classList.remove("drag-over");
    const files = e.dataTransfer && e.dataTransfer.files;
    if (!files || files.length === 0) return;
    const file = files[0];
    if (!file.name.endsWith(".torrent") && file.type !== "application/x-bittorrent") {
      setMessage("Drop a .torrent file", false);
      return;
    }
    const dt = new DataTransfer();
    dt.items.add(file);
    fileInput.files = dt.files;
    byId("torrentFileName").textContent = file.name;
  });
}

bindEvents();
connectEvents();
