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

var refreshInFlight = false;
var lastTaskSignature = "";
var detailTaskId = 0;
var detailRefreshTimer = 0;
var detailInFlight = false;
var eventsSource = null;
var usingSse = false;
var fallbackPollTimer = 0;

var settingsElementIds = [
  "maxPeers", "listenPort", "listenPortEnd", "downloadLimit", "uploadLimit", "maxConcurrent",
  "enableDht", "enablePex", "enableUpnp"
];

function kvRows(obj) {
  return Object.entries(obj)
    .map(([k, v]) => `<div class="kv"><span class="k">${k}</span><span class="v">${v}</span></div>`)
    .join("");
}

function setMessage(message, ok) {
  const el = document.getElementById("controlMsg");
  if (!el) return;
  el.innerHTML = ok ? `<span class="ok">${message}</span>` : `<span class="err">${message}</span>`;
}

function setSyncState(ok, detail) {
  var el = document.getElementById("syncState");
  if (!el) return;
  var now = new Date();
  el.className = "sync-state " + (ok ? "sync-ok" : "sync-error");
  el.textContent = ok ? "在线 · " + now.toLocaleTimeString() : "离线 · " + (detail || "同步失败");
}

function isEditingSettings() {
  var active = document.activeElement;
  return !!active && settingsElementIds.indexOf(active.id) >= 0;
}

function setInputValueIfIdle(id, value) {
  var el = document.getElementById(id);
  if (!el || document.activeElement === el) return;
  el.value = value == null ? "" : value;
}

function setCheckboxIfIdle(id, checked) {
  var el = document.getElementById(id);
  if (!el || document.activeElement === el) return;
  el.checked = checked;
}

function formatDuration(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "-";
  if (seconds < 60) return Math.round(seconds) + " 秒";
  if (seconds < 3600) return Math.round(seconds / 60) + " 分钟";
  return (seconds / 3600).toFixed(1) + " 小时";
}

function formatCountdown(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "-";
  if (seconds <= 0) return "now";
  if (seconds < 60) return Math.ceil(seconds) + " 秒";
  if (seconds < 3600) return Math.ceil(seconds / 60) + " 分钟";
  return Math.ceil(seconds / 3600) + " 小时";
}

function taskSignature(tasks) {
  return JSON.stringify((tasks || []).map(function(t) {
    return [t.id, t.status, t.running, t.progress, t.download_speed, t.peer_count, t.last_error, t.updated_at_ms];
  }));
}

function handleOverviewData(data) {
  setSyncState(true, usingSse ? "SSE" : "poll");

  document.getElementById("overview").innerHTML = kvRows({
    app: data.app_name,
    worker: data.worker_index,
    services: data.service_count,
    last_event_ms: data.last_event_ms
  });

  document.getElementById("bt").innerHTML = kvRows({
    running: data.bt.running,
    active_tasks: (data.bt.active_count || 0) + '/' + (data.bt.max_concurrent || 3),
    metadata_mode: data.bt.metadata_mode ? "yes (fetching info)" : "no",
    complete: data.bt.complete,
    peers: data.bt.peer_count,
    max_peers: data.bt.max_peers,
    listen_port: (data.bt.listen_port || 6881) + '-' + (data.bt.listen_port_end || 6999),
    download_limit: (data.bt.download_limit_kbps || 0) + " KB/s",
    download_limit_active: data.bt.download_limit_active ? "yes" : "no",
    upload_limit: (data.bt.upload_limit_kbps || 0) + " KB/s",
    upload_limit_active: data.bt.upload_limit_active ? "yes" : "no",
    downloaded: formatBytes(data.bt.downloaded_bytes || 0),
    uploaded: formatBytes(data.bt.uploaded_bytes || 0),
    ratio: Number(data.bt.ratio || 0).toFixed(2),
    progress: ((data.bt.progress || 0) * 100).toFixed(1) + "%",
    pieces: (data.bt.pieces_downloaded || 0) + " / " + (data.bt.pieces_total || 0),
    speed_now: formatBytes(data.bt.download_speed || 0) + "/s",
    upload_now: formatBytes(data.bt.upload_speed || 0) + "/s",
    speed_avg: formatBytes(data.bt.avg_download_speed || 0) + "/s",
    dht: data.bt.dht_running ? ("running, nodes=" + (data.bt.dht_nodes || 0)) : "off",
    nat: data.bt.nat_mapped
      ? ("mapped " + (data.bt.nat_external_ip || "?") + ":" + (data.bt.nat_mapped_port || 0))
      : (data.bt.nat_igd_discovered ? "IGD found, not mapped" : "not mapped"),
    last_info_hash: data.bt.last_info_hash || "-",
    last_torrent: data.bt.last_torrent_name || "-",
    peer_events: data.bt.peer_connected_total,
    piece_events: data.bt.piece_completed_total,
    torrent_events: data.bt.torrent_completed_total
  });

  document.getElementById("raw").textContent = JSON.stringify({
    service_states: data.service_states,
    event_counters: data.event_counters,
    recent_events: data.recent_events
  }, null, 2);

  document.getElementById("taskHistory").innerHTML = renderHistory(data.bt_task_history || []);

  if (data.bt && data.bt.tasks) {
    var errorTasks = data.bt.tasks.filter(function(t) { return t.status === 'error'; });
    var errorBanner = document.getElementById('errorBanner');
    if (errorTasks.length > 0) {
      var errorHtml = '<strong>' + errorTasks.length + ' 个任务失败</strong><ul>';
      for (var i = 0; i < errorTasks.length; i++) {
        var et = errorTasks[i];
        errorHtml += '<li>#' + et.id + ' ' + escHtml(et.name || et.info_hash || '未知') + ' — ' + escHtml(formatError(et.last_error)) + ' <button class="retry" style="font-size:11px;padding:2px 8px;margin-left:6px" onclick="retryTaskByIdValue(' + et.id + ')">重试</button></li>';
      }
      errorHtml += '</ul>';
      errorBanner.innerHTML = errorHtml;
      errorBanner.style.display = 'block';
    } else {
      errorBanner.style.display = 'none';
    }
    document.getElementById("taskSummary").innerHTML = renderTaskSummary(data.bt.tasks, data.bt.active_count || 0, data.bt.max_concurrent || 3);
    var sig = taskSignature(data.bt.tasks);
    if (sig !== lastTaskSignature) {
      document.getElementById("taskList").innerHTML = renderTaskTable(data.bt.tasks, data.bt.active_count || 0, data.bt.max_concurrent || 3);
      lastTaskSignature = sig;
    }
  }

  if (!isEditingSettings()) {
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

async function applyImportedConfig(config) {
  const settingsPayload = {};
  if (config && config.bt && typeof config.bt === "object") {
    if (Number.isFinite(Number(config.bt.max_peers))) settingsPayload.max_peers = Number(config.bt.max_peers);
    if (Number.isFinite(Number(config.bt.listen_port))) settingsPayload.listen_port = Number(config.bt.listen_port);
    if (Number.isFinite(Number(config.bt.download_limit_kbps))) settingsPayload.download_limit_kbps = Number(config.bt.download_limit_kbps);
    if (Number.isFinite(Number(config.bt.upload_limit_kbps))) settingsPayload.upload_limit_kbps = Number(config.bt.upload_limit_kbps);
    if (typeof config.bt.enable_dht === "boolean") settingsPayload.enable_dht = config.bt.enable_dht;
    if (typeof config.bt.enable_pex === "boolean") settingsPayload.enable_pex = config.bt.enable_pex;
    if (typeof config.bt.enable_upnp === "boolean") settingsPayload.enable_upnp = config.bt.enable_upnp;
    if (Number.isFinite(Number(config.bt.max_concurrent_downloads))) settingsPayload.max_concurrent_downloads = Number(config.bt.max_concurrent_downloads);
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

function renderTaskTable(tasks, activeCount, maxConcurrent) {
  if (!tasks || !tasks.length) {
    return '<div class="empty-hint">暂无任务</div>';
  }
  let html = '<table class="task-table"><thead><tr>'
    + '<th>ID</th><th>名称</th><th>大小</th><th>进度</th><th>速度</th><th>ETA</th><th>状态</th><th>Peers</th><th>类型</th><th>操作</th>'
    + '</tr></thead><tbody>';
  for (const t of tasks) {
    const isActive = t.active || t.running;
    const progress = t.progress != null && t.progress > 0 ? (t.progress * 100).toFixed(1) + '%' : '-';
    const size = t.total_bytes > 0 ? formatBytes(t.total_bytes) : '-';
    const downloaded = t.downloaded_bytes || 0;
    const remaining = Math.max(0, (t.total_bytes || 0) - downloaded);
    const eta = t.download_speed > 0 && remaining > 0 ? formatDuration(remaining / t.download_speed) : '-';
    const speed = t.status === 'seeding' && t.uploaded_bytes > 0
      ? '↑ ' + formatBytes(t.upload_speed || 0) + '/s'
      : t.download_speed > 0 ? '↓ ' + formatBytes(t.download_speed) + '/s' : '-';
    const statusLabel = formatStatus(t.status || (t.running ? 'running' : 'stopped'));
    const statusClass = t.status === 'error' ? 'status-error'
      : t.status === 'seeding' ? 'status-seeding'
      : t.status === 'completed' ? 'status-completed'
      : t.status === 'paused' ? 'status-stopped'
      : t.running ? 'status-running' : 'status-stopped';
    const progressPct = t.progress != null ? Math.round(t.progress * 100) : 0;
    const startBtn = (!t.running && t.status !== 'error' && t.status !== 'seeding' && t.status !== 'paused') ? '<button onclick="startTaskByIdValue(' + t.id + ')">启动</button>' : '';
    const resumeBtn = t.status === 'paused' ? '<button onclick="resumeTaskByIdValue(' + t.id + ')">继续</button>' : '';
    const retryBtn = t.status === 'error' ? '<button class="retry" onclick="retryTaskByIdValue(' + t.id + ')">重试</button>' : '';
    const pauseBtn = t.running ? '<button onclick="pauseTaskByIdValue(' + t.id + ')">暂停</button>' : '';
    const stopBtn = (t.running || t.status === 'seeding') ? '<button onclick="stopTaskByIdValue(' + t.id + ')">停止</button>' : '';
    const detailBtn = '<button onclick="showTaskDetail(' + t.id + ')">详情</button>';
    const taskType = t.magnet_uri ? '磁力链' : (t.has_torrent_data ? '上传' : '文件');
    const ratio = t.ratio != null ? Number(t.ratio).toFixed(2) : '-';
    const statusCell = t.status === 'error' && t.last_error
      ? '<span class="' + statusClass + '">' + statusLabel + '</span><div class="error-detail">' + escHtml(formatError(t.last_error)) + '</div>'
      : '<span class="' + statusClass + '">' + statusLabel + '</span>';
    html += '<tr class="' + (isActive ? 'row-active' : '') + '">'
      + '<td>' + t.id + '</td>'
      + '<td class="cell-name" title="' + escHtml(t.name || t.info_hash || '-') + '">' + escHtml(t.name || t.info_hash || '-') + '</td>'
      + '<td>' + size + '</td>'
      + '<td><div class="progress-cell"><div class="progress-bar" style="width:' + progressPct + '%"></div><span class="progress-text">' + progress + '</span></div></td>'
      + '<td>' + speed + '<div class="subtle">R ' + ratio + '</div></td>'
      + '<td>' + eta + '</td>'
      + '<td>' + statusCell + '</td>'
      + '<td>' + (t.peer_count != null ? t.peer_count : '-') + '</td>'
      + '<td>' + taskType + '</td>'
      + '<td class="cell-actions">'
      + detailBtn + retryBtn + resumeBtn + startBtn + pauseBtn + stopBtn
      + '<button class="warn" onclick="removeTaskByIdValue(' + t.id + ')">删除</button>'
      + '</td></tr>';
  }
  html += '</tbody></table>';
  return html;
}

function renderTaskSummary(tasks, activeCount, maxConcurrent) {
  tasks = tasks || [];
  var counts = { queued: 0, running: 0, paused: 0, seeding: 0, completed: 0, stopped: 0, error: 0 };
  var totalSpeed = 0;
  var totalUploadSpeed = 0;
  var totalRemaining = 0;
  tasks.forEach(function(t) {
    var status = t.status || (t.running ? 'running' : 'stopped');
    counts[status] = (counts[status] || 0) + 1;
    totalSpeed += t.download_speed || 0;
    totalUploadSpeed += t.upload_speed || 0;
    if ((t.total_bytes || 0) > 0) totalRemaining += Math.max(0, (t.total_bytes || 0) - (t.downloaded_bytes || 0));
  });
  var eta = totalSpeed > 0 && totalRemaining > 0 ? formatDuration(totalRemaining / totalSpeed) : '-';
  return '<span class="summary-pill">运行 ' + activeCount + '/' + maxConcurrent + '</span>'
    + '<span class="summary-pill">排队 ' + (counts.queued || 0) + '</span>'
    + '<span class="summary-pill">暂停 ' + (counts.paused || 0) + '</span>'
    + '<span class="summary-pill">做种 ' + (counts.seeding || 0) + '</span>'
    + '<span class="summary-pill">失败 ' + (counts.error || 0) + '</span>'
    + '<span class="summary-pill">下载 ' + formatBytes(totalSpeed) + '/s</span>'
    + '<span class="summary-pill">上传 ' + formatBytes(totalUploadSpeed) + '/s</span>'
    + '<span class="summary-pill">预计剩余 ' + eta + '</span>';
}

function formatStatus(status) {
  var map = {
    queued: '排队中',
    loaded: '已加载',
    running: '运行中',
    paused: '已暂停',
    seeding: '做种中',
    error: '失败',
    stopped: '已停止',
    completed: '已完成'
  };
  return map[status] || status || '未知';
}

function formatError(error) {
  if (!error) return '';
  var map = {
    load_torrent_failed: '加载 torrent 失败',
    load_magnet_failed: '加载磁力链失败',
    start_task_failed: '启动任务失败',
    task_not_active: '该任务未在运行中',
    task_completed: '任务已完成',
    add_task_failed: '添加任务失败',
    invalid_torrent_data: '无效的 torrent 数据',
    save_torrent_failed: '保存 torrent 文件失败',
    torrent_data_empty: 'torrent 数据为空',
    add_magnet_task_failed: '添加磁力链任务失败'
  };
  return map[error] || error;
}

function renderTaskDetail(data) {
  var html = '';

  html += '<div class="detail-metrics">'
    + '<span>状态: ' + formatStatus(data.running ? 'running' : (data.active ? 'loaded' : 'stopped')) + '</span>'
    + '<span>元数据: ' + (data.metadata_mode ? '获取中' : '已就绪') + '</span>'
    + '<span>Info Hash: ' + escHtml(data.info_hash || '-') + '</span>'
    + '</div>';

  html += '<div class="detail-section"><h4>文件列表</h4>';
  if (data.files && data.files.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>路径</th><th>大小</th><th>偏移</th></tr></thead><tbody>';
    for (var f of data.files) {
      html += '<tr><td>' + escHtml(f.name || '-') + '</td><td>' + formatBytes(f.length || 0) + '</td><td>' + (f.offset || 0) + '</td></tr>';
    }
    html += '</tbody></table>';
  } else {
    html += '<div class="empty-hint">暂无文件信息（磁力链可能仍在获取元数据）</div>';
  }
  html += '</div>';

  if (data.pieces) {
    html += '<div class="detail-section"><h4>Pieces 进度</h4>';
    var p = data.pieces;
    var total = p.total || 0;
    if (total > 0) {
      var donePct = total > 0 ? Math.round((p.done / total) * 100) : 0;
      var activePct = total > 0 ? Math.round((p.active / total) * 100) : 0;
      var pendingPct = Math.max(0, 100 - donePct - activePct);
      html += '<div class="pieces-bar">';
      if (p.done > 0) html += '<div class="seg seg-done" style="width:' + donePct + '%">' + p.done + '</div>';
      if (p.active > 0) html += '<div class="seg seg-active" style="width:' + activePct + '%">' + p.active + '</div>';
      if (p.pending > 0) html += '<div class="seg seg-pending" style="width:' + pendingPct + '%">' + p.pending + '</div>';
      html += '</div>';
      html += '<div class="detail-note">已完成 ' + p.done + ' / 下载中 ' + p.active + ' / 等待 ' + p.pending + ' / 共 ' + total + (p.endgame ? ' (收尾阶段)' : '') + '</div>';
    } else {
      html += '<div class="empty-hint">暂无 piece 信息</div>';
    }
    html += '</div>';
  }

  html += '<div class="detail-section"><h4>Peers (' + (data.peers ? data.peers.length : 0) + ')</h4>';
  if (data.peers && data.peers.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>地址</th><th>状态</th><th>下载</th><th>上传</th><th>Pieces</th><th>标志</th></tr></thead><tbody>';
    for (var peer of data.peers) {
      var stateClass = 'tag-' + (peer.state || 'idle');
      var flags = [];
      if (peer.peer_choking) flags.push('被 choke');
      if (!peer.am_interested) flags.push('无兴趣');
      if (peer.snubbed) flags.push('snub');
      html += '<tr>'
        + '<td>' + escHtml(peer.ip || '?') + ':' + (peer.port || 0) + '</td>'
        + '<td><span class="tag ' + stateClass + '">' + escHtml(peer.state || '?') + '</span></td>'
        + '<td>' + formatBytes(peer.download_rate || 0) + '/s</td>'
        + '<td>' + formatBytes(peer.upload_rate || 0) + '/s</td>'
        + '<td>' + (peer.pieces_have || 0) + '/' + (peer.pieces_total || 0) + '</td>'
        + '<td>' + (flags.length ? flags.join(', ') : '-') + '</td>'
        + '</tr>';
    }
    html += '</tbody></table>';
  } else {
    html += '<div class="empty-hint">暂无连接的 Peer</div>';
  }
  html += '</div>';

  html += '<div class="detail-section"><h4>Trackers</h4>';
  if (data.trackers && data.trackers.length > 0) {
    html += '<table class="detail-table"><thead><tr><th>URL</th><th>层级</th><th>来源</th><th>最近状态</th><th>Peers</th><th>间隔</th><th>下次</th></tr></thead><tbody>';
    for (var trk of data.trackers) {
      var last = trk.last_announce_ms ? new Date(trk.last_announce_ms).toLocaleTimeString() : '-';
      var state = trk.last_announce_ms
        ? (trk.last_success ? '<span class="status-running">成功</span>' : '<span class="status-error">失败</span>')
        : '<span class="status-stopped">未 announce</span>';
      if (trk.error) state += '<div class="error-detail">' + escHtml(formatError(trk.error)) + '</div>';
      var nextAnnounce = '-';
      if (trk.last_announce_ms && trk.interval) {
        var elapsed = (Date.now() - trk.last_announce_ms) / 1000;
        nextAnnounce = formatCountdown(trk.interval - elapsed);
      }
      html += '<tr>'
        + '<td style="word-break:break-all">' + escHtml(trk.url || '-') + '</td>'
        + '<td>' + (trk.tier >= 0 ? trk.tier : (trk.tier === -2 ? 'magnet' : '主')) + '</td>'
        + '<td>' + escHtml(trk.source || 'torrent') + '</td>'
        + '<td>' + state + '<div class="subtle">' + last + '</div></td>'
        + '<td>' + (trk.peers || 0) + '</td>'
        + '<td>' + (trk.interval ? (trk.interval + 's') : '-') + '</td>'
        + '<td>' + nextAnnounce + '</td>'
        + '</tr>';
    }
    html += '</tbody></table>';
  } else {
    html += '<div class="empty-hint">暂无 Tracker 信息</div>';
  }
  html += '</div>';
  return html;
}

async function refreshTaskDetail() {
  if (!detailTaskId || detailInFlight) return;
  detailInFlight = true;
  var sync = document.getElementById('detailSync');
  try {
    var data = await requestJson('/admin/api/bt/tasks/detail?task_id=' + detailTaskId);
    document.getElementById('detailBody').innerHTML = renderTaskDetail(data);
    if (sync) sync.textContent = '自动刷新 · ' + new Date().toLocaleTimeString();
  } catch (error) {
    document.getElementById('detailBody').innerHTML = '<div class="empty-hint err">加载失败: ' + escHtml(error.message || String(error)) + '</div>';
    if (sync) sync.textContent = '刷新失败';
  } finally {
    detailInFlight = false;
  }
}

async function showTaskDetail(taskId) {
  var modal = document.getElementById('detailModal');
  var title = document.getElementById('detailTitle');
  var body = document.getElementById('detailBody');
  detailTaskId = taskId;
  title.textContent = '任务 #' + taskId + ' 详情';
  body.innerHTML = '<div class="empty-hint">加载中...</div>';
  modal.style.display = 'flex';
  await refreshTaskDetail();
  clearInterval(detailRefreshTimer);
  detailRefreshTimer = setInterval(refreshTaskDetail, 1000);
}

function closeTaskDetail() {
  detailTaskId = 0;
  clearInterval(detailRefreshTimer);
  detailRefreshTimer = 0;
  document.getElementById("detailModal").style.display = "none";
}

function formatBytes(bytes) {
  bytes = Number(bytes) || 0;
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1073741824).toFixed(2) + ' GB';
}

function renderHistory(history) {
  if (!history || !history.length) {
    return '<div class="empty-hint">暂无历史记录</div>';
  }
  var html = '<table class="detail-table"><thead><tr><th>时间</th><th>动作</th><th>名称</th><th>保存路径</th><th>状态</th></tr></thead><tbody>';
  for (var item of history) {
    var ts = item.ts ? new Date(item.ts).toLocaleString() : '-';
    html += '<tr>'
      + '<td>' + escHtml(ts) + '</td>'
      + '<td>' + escHtml(item.action || '-') + '</td>'
      + '<td>' + escHtml(item.name || item.info_hash || '-') + '</td>'
      + '<td>' + escHtml(item.save_path || '-') + '</td>'
      + '<td>' + (item.running ? '<span class="status-running">运行</span>' : '<span class="status-stopped">停止</span>') + '</td>'
      + '</tr>';
  }
  html += '</tbody></table>';
  return html;
}

function escHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

async function startTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "task started", true);
    await refresh();
  } catch (error) {
    var msg = error.message || String(error);
    if (msg.indexOf("max_concurrent_reached") >= 0) {
      msg = "并发下载数已满，无法启动";
    }
    setMessage(msg, false);
  }
}

async function retryTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "重试成功", true);
    await refresh();
  } catch (error) {
    var msg = error.message || String(error);
    if (msg.indexOf("max_concurrent_reached") >= 0) {
      msg = "并发下载数已满，无法重试";
    }
    setMessage("重试失败: " + msg, false);
  }
}

async function stopTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/stop", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "task stopped", true);
    await refresh();
  } catch (error) {
    var msg = error.message || String(error);
    if (msg.indexOf("task_not_active") >= 0) {
      msg = "该任务未在运行中";
    }
    setMessage(msg, false);
  }
}

async function pauseTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/pause", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "task paused", true);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function resumeTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/resume", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "task resumed", true);
    await refresh();
  } catch (error) {
    var msg = error.message || String(error);
    if (msg.indexOf("max_concurrent_reached") >= 0) {
      msg = "并发下载数已满，无法继续";
    }
    setMessage(msg, false);
  }
}

async function removeTaskByIdValue(id) {
  try {
    const data = await requestJson("/admin/api/bt/tasks/remove", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: id })
    });
    setMessage(data.message || "task removed", true);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
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
    const tasks = data.items || data;
    const activeCount = data.active_count || 0;
    const maxConcurrent = data.max_concurrent || 3;
    document.getElementById("taskSummary").innerHTML = renderTaskSummary(Array.isArray(tasks) ? tasks : [], activeCount, maxConcurrent);
    document.getElementById("taskList").innerHTML = renderTaskTable(Array.isArray(tasks) ? tasks : [], activeCount, maxConcurrent);
    lastTaskSignature = taskSignature(Array.isArray(tasks) ? tasks : []);
  } catch (error) {
    setMessage(error.message || String(error), false);
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
    eventsSource = new EventSource('/admin/api/bt/events');
  } catch (_) {
    startFallbackPolling(2000);
    refresh();
    return;
  }

  eventsSource.addEventListener('ready', function() {
    usingSse = true;
    clearInterval(fallbackPollTimer);
    refresh();
  });

  eventsSource.addEventListener('overview', function(event) {
    usingSse = true;
    clearInterval(fallbackPollTimer);
    try {
      handleOverviewData(JSON.parse(event.data));
    } catch (error) {
      setSyncState(false, 'SSE 数据错误');
    }
  });

  eventsSource.onerror = function() {
    usingSse = false;
    setSyncState(false, 'SSE 断开，轮询兜底');
    startFallbackPolling(2000);
  };
}

async function startTaskById() {
  const taskId = Number(document.getElementById("taskStartId").value || 0);
  if (!taskId) {
    setMessage("请填写有效任务 ID", false);
    return;
  }

  try {
    const data = await requestJson("/admin/api/bt/tasks/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: taskId })
    });
    var msg = data.message || "task started";
    setMessage(msg, true);
    await refresh();
  } catch (error) {
    var msg = error.message || String(error);
    if (msg.indexOf("max_concurrent_reached") >= 0) {
      msg = "并发下载数已满，无法启动";
    }
    setMessage(msg, false);
  }
}

async function stopTaskById() {
  const taskId = Number(document.getElementById("taskStopId").value || 0);
  if (!taskId) {
    setMessage("请填写有效任务 ID", false);
    return;
  }

  try {
    const data = await requestJson("/admin/api/bt/tasks/stop", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: taskId })
    });
    setMessage(data.message || "task stopped", true);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function removeTaskById() {
  const taskId = Number(document.getElementById("taskRemoveId").value || 0);
  if (!taskId) {
    setMessage("请填写有效删除任务 ID", false);
    return;
  }

  try {
    const data = await requestJson("/admin/api/bt/tasks/remove", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ task_id: taskId })
    });
    setMessage(data.message || "task removed", true);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function loadHistory() {
  const page = Number(document.getElementById("historyPage").value || 1);
  const pageSize = Number(document.getElementById("historyPageSize").value || 20);
  try {
    const data = await requestJson(`/admin/api/bt/history?page=${page}&page_size=${pageSize}`);
    document.getElementById("taskHistory").innerHTML = renderHistory(data.items || []);
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function exportConfigTemplate() {
  try {
    const data = await requestJson("/admin/api/overview");
    const cfg = {
      app_name: data.app_name || "bt_downloader",
      admin_port: 18080,
      save_path: ".",
      enable_keep_alive: true,
      enable_cors: true,
      bt: {
        max_peers: data.bt.max_peers || 50,
        listen_port: data.bt.listen_port || 6881,
        download_limit_kbps: data.bt.download_limit_kbps || 0,
        upload_limit_kbps: data.bt.upload_limit_kbps || 0,
        max_concurrent_downloads: data.bt.max_concurrent || 3,
        enable_dht: data.bt.enable_dht !== false,
        enable_pex: data.bt.enable_pex !== false,
        enable_upnp: data.bt.enable_upnp !== false
      }
    };

    downloadTextFile("bt_downloader.config.template.json", JSON.stringify(cfg, null, 2));
    setMessage("配置模板已导出", true);
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function importConfigTemplate() {
  const fileInput = document.getElementById("btImportConfigFile");
  const file = fileInput && fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
  if (!file) {
    setMessage("请先选择配置文件", false);
    return;
  }

  try {
    const text = await file.text();
    const config = JSON.parse(text);
    setMessage("正在应用导入配置...", true);
    await applyImportedConfig(config);
    setMessage("导入配置已应用", true);
    fileInput.value = "";
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function control(action) {
  setMessage("处理中...", true);
  try {
    const data = await requestJson("/admin/api/bt/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action })
    });
    setMessage(data.message || "ok", true);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function loadTorrent() {
  const torrentPath = document.getElementById("torrentPath").value.trim();
  const savePath = document.getElementById("savePath").value.trim();
  if (!torrentPath) {
    setMessage("请先填写 torrent 文件路径", false);
    return;
  }

  setMessage("处理中...", true);
  try {
    const data = await requestJson("/admin/api/bt/torrent", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ torrent_path: torrentPath, save_path: savePath })
    });
    setMessage(data.queued ? "任务已加入排队（并发下载数已满）" : (data.message || "torrent loaded"), !data.queued);
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function uploadTorrent() {
  const fileInput = document.getElementById("torrentFile");
  const file = fileInput && fileInput.files && fileInput.files[0] ? fileInput.files[0] : null;
  if (!file) {
    setMessage("请先选择 .torrent 文件", false);
    return;
  }

  const savePath = document.getElementById("savePath").value.trim();
  const formData = new FormData();
  formData.append("torrent", file);
  if (savePath) {
    formData.append("save_path", savePath);
  }

  setMessage("上传中...", true);
  try {
    const data = await requestJson("/admin/api/bt/upload", {
      method: "POST",
      body: formData
    });
    setMessage(data.queued ? "任务已加入排队（并发下载数已满）" : (data.message || "torrent uploaded"), !data.queued);
    fileInput.value = "";
    document.getElementById("torrentFileName").textContent = "";
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function loadMagnet() {
  const magnetUri = document.getElementById("magnetUri").value.trim();
  const savePath = document.getElementById("savePath").value.trim();
  if (!magnetUri) {
    setMessage("请先填写 magnet 链接", false);
    return;
  }

  setMessage("处理中...", true);
  try {
    const data = await requestJson("/admin/api/bt/magnet", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ magnet_uri: magnetUri, save_path: savePath })
    });
    setMessage(data.queued ? "任务已加入排队（并发下载数已满）" : (data.message || "magnet task added"), !data.queued);
    document.getElementById("magnetUri").value = "";
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

async function applySettings() {
  const maxPeersRaw = document.getElementById("maxPeers").value.trim();
  const listenPortRaw = document.getElementById("listenPort").value.trim();
  const listenPortEndRaw = document.getElementById("listenPortEnd").value.trim();
  const downloadLimitRaw = document.getElementById("downloadLimit").value.trim();
  const uploadLimitRaw = document.getElementById("uploadLimit").value.trim();
  const maxConcurrentRaw = document.getElementById("maxConcurrent").value.trim();
  const enableDht = document.getElementById("enableDht").checked;
  const enablePex = document.getElementById("enablePex").checked;
  const enableUpnp = document.getElementById("enableUpnp").checked;
  const payload = {};
  if (maxPeersRaw) payload.max_peers = Number(maxPeersRaw);
  if (listenPortRaw) payload.listen_port = Number(listenPortRaw);
  if (listenPortEndRaw) payload.listen_port_end = Number(listenPortEndRaw);
  if (downloadLimitRaw) payload.download_limit_kbps = Number(downloadLimitRaw);
  if (uploadLimitRaw) payload.upload_limit_kbps = Number(uploadLimitRaw);
  if (maxConcurrentRaw) payload.max_concurrent_downloads = Number(maxConcurrentRaw);
  payload.enable_dht = enableDht;
  payload.enable_pex = enablePex;
  payload.enable_upnp = enableUpnp;

    setMessage("处理中...", true);
  try {
    const data = await requestJson("/admin/api/bt/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    });
    if (data.nat_restart_required) {
      setMessage((data.message || "settings updated") + " (DHT/PEX/UPnP 变更需要重启任务后生效)", true);
    } else {
      setMessage(data.message || "settings updated", true);
    }
    await refresh();
  } catch (error) {
    setMessage(error.message || String(error), false);
  }
}

document.getElementById("btStart").addEventListener("click", () => control("start"));
document.getElementById("btStop").addEventListener("click", () => control("stop"));
document.getElementById("btLoad").addEventListener("click", loadTorrent);
document.getElementById("btUpload").addEventListener("click", uploadTorrent);
document.getElementById("btMagnet").addEventListener("click", loadMagnet);
document.getElementById("btApplySettings").addEventListener("click", applySettings);
document.getElementById("btExportConfig").addEventListener("click", exportConfigTemplate);
document.getElementById("btImportConfigApply").addEventListener("click", importConfigTemplate);
document.getElementById("historyLoad").addEventListener("click", loadHistory);
document.getElementById("taskRefreshBtn").addEventListener("click", refreshTasks);
document.getElementById("taskStartBtn").addEventListener("click", startTaskById);
document.getElementById("taskStopBtn").addEventListener("click", stopTaskById);
document.getElementById("taskRemoveBtn").addEventListener("click", removeTaskById);

document.getElementById("detailClose").addEventListener("click", function() {
  closeTaskDetail();
});
document.getElementById("detailModal").addEventListener("click", function(e) {
  if (e.target === this) closeTaskDetail();
});

document.getElementById("torrentFile").addEventListener("change", function() {
  const name = this.files && this.files[0] ? this.files[0].name : "";
  document.getElementById("torrentFileName").textContent = name;
});

(function setupDropZone() {
  const zone = document.getElementById("dropZone");
  const fileInput = document.getElementById("torrentFile");
  if (!zone || !fileInput) return;

  zone.addEventListener("click", function() {
    fileInput.click();
  });

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
    if (files && files.length > 0) {
      const file = files[0];
      if (file.name.endsWith(".torrent") || file.type === "application/x-bittorrent") {
        const dt = new DataTransfer();
        dt.items.add(file);
        fileInput.files = dt.files;
        document.getElementById("torrentFileName").textContent = file.name;
      } else {
        setMessage("请拖入 .torrent 文件", false);
      }
    }
  });
})();

connectEvents();
