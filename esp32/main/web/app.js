'use strict';
const byId = id => document.getElementById(id);
let state = null, pendingCommand = false, authSignature = '';
async function command(action, fields = {}) {
  if (!state?.management_open || pendingCommand) return;
  pendingCommand = true;
  try {
    const response = await fetch('/api/manage', {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({action, token: state.token, ...fields}), signal: AbortSignal.timeout(3000)});
    if (!response.ok) throw new Error(await response.text());
    byId('command-result').textContent = '请求已受理；以设备操作结果和槽位更新为准。';
  } catch (error) { byId('command-result').textContent = `操作未确认：${error.message}。请刷新状态后再试，不要假设删除成功。`; }
  finally { pendingCommand = false; }
}
function actionButton(label, handler, enabled, reason = '') {
  const button = text('button', label); button.disabled = !enabled;
  if (!enabled && reason) { button.title = reason; button.setAttribute('aria-label', `${label}：${reason}`); }
  button.addEventListener('click', handler); return button;
}
function managementBlock(data, pc = false, deleting = false) {
  if (!data.management_open) return '未授权：长按 ESP32 BOOT 3 秒后松开';
  if (data.trial) return '正在录入设备：先取消或等待完成';
  if (data.input_storage_fault) return '输入存储异常，管理已锁定';
  if (data.deletion_pending) return '正在处理已确认的删除，请等待结果';
  if (pc && (!data.pico_fresh || data.storage_fault)) return '电脑状态未知，不能删除';
  if (!deleting && data.auto_enrollment) return '自动录入中：先点“停止自动录入／手动管理”';
  if (!deleting && !pc && data.scanning) return '扫描中：等待完成或停止扫描';
  return '';
}
function deleteButton(node, label, address, fields, data, pc = false, saved = true) {
  const reason = saved ? managementBlock(data, pc, true) : '此槽尚未保存设备';
  node.append(actionButton(pc ? '删除此电脑' : '删除此设备', () => {
    if (confirm(`删除 ${label}（${address}）及其配对记录？此操作不可撤销。`))
      command(pc ? 'pc-delete' : 'delete', {...fields, address});
  }, saved && !reason, reason));
  if (reason) node.append(text('p', reason, 'control-note'));
  else if (data.auto_enrollment || data.scanning)
    node.append(text('p', '确认删除后会先停止自动录入／扫描，再删除指定记录', 'control-note'));
}
function text(tag, value, className) {
  const node = document.createElement(tag);
  node.textContent = value;
  if (className) node.className = className;
  return node;
}
function badge(node, label, kind) { node.textContent = label; node.className = `badge ${kind}`; }
function card(title, label, kind, address, details, selected = false) {
  const node = text('article', '', `card${selected ? ' selected' : ''}`);
  const header = text('header', '');
  header.append(text('h3', title), text('span', label, `badge ${kind}`));
  node.append(header, text('p', address || '没有设备地址'), text('p', details, 'details'));
  return node;
}
function unknown(message) {
  state = null;
  authSignature = '';
  for (const id of ['manual', 'scan-classic', 'scan-ble', 'cancel']) byId(id).disabled = true;
  byId('records').replaceChildren(); byId('candidates').replaceChildren(); byId('auth').replaceChildren();
  byId('input-events').replaceChildren();
  badge(byId('health'), '状态不可用', 'warning');
  byId('selected').textContent = '状态未知';
  byId('uart').textContent = '状态未知';
  byId('freshness').textContent = '通信未确认';
  byId('error').hidden = false;
  byId('error').textContent = message;
  byId('inputs').replaceChildren(...[1, 2].map(i => card(`输入 ${i}`, '状态未知', 'unknown', '', '没有最新有效数据')));
  byId('computers').replaceChildren(...[1, 2, 3, 4, 5, 6, 7].map(i => card(`PC${i}`, '状态未知', 'unknown', '', '不能使用旧数据判断是否在线')));
}
function render(data) {
  if (data.schema !== 1 || typeof data.uart_online !== 'boolean' || typeof data.pico_fresh !== 'boolean' ||
      !Array.isArray(data.inputs) || data.inputs.length !== 2 ||
      (data.pico_fresh && (!Array.isArray(data.computers) || data.computers.length !== 7 ||
        !Number.isInteger(data.selected_pc) || data.selected_pc < 1 || data.selected_pc > 7))) {
    throw new Error('设备返回了不兼容的状态数据');
  }
  state = data;
  const current = data.pico_fresh && data.uart_online && !data.storage_fault;
  badge(byId('health'), current ? '状态已更新' : '电脑状态未知', current ? 'ready' : 'warning');
  byId('selected').textContent = current ? `PC${data.selected_pc}${data.switching ? ' · 切换中' : ''}` : '状态未知';
  byId('uart').textContent = data.uart_online ? '在线' : '通信中断';
  byId('freshness').textContent = current ? `${(data.pico_age_ms / 1000).toFixed(1)} 秒前` : '过期 / 未收到';
  byId('error').hidden = current && !data.storage_fault && !data.input_storage_fault;
  byId('error').textContent = data.input_storage_fault ? 'ESP32 输入存储校验失败，修改已锁定；不要继续删除或重新配对。' :
    data.storage_fault ? 'Pico 配对存储异常，电脑就绪状态不可依赖。' :
    '没有最新的 Pico 电脑状态：可能是串口断线、Pico 未运行新固件或状态已过期。';
  byId('inputs').replaceChildren(...data.inputs.map(input => {
    const role = input.slot === 1 ? '键盘' : '鼠标';
    const label = !input.saved ? '未录入' : input.paused ? '已暂停' : input.ready ? '可转发' :
      input.connected ? '已连接，未就绪' : input.pending ? '连接中' : '离线';
    const details = input.saved ? `${input.transport} · ${input.bonded ? '有配对记录' : '无配对记录'} · ${input.encrypted ? '已加密' : '未加密'} · HID ${input.hid ? '就绪' : '未就绪'} · 报告 ${input.reports}${input.interval_ms == null ? '' : ` · ${input.interval_ms} ms`}` : '长按 ESP32 BOOT 授权，然后在下方扫描并录入新键鼠';
    const node = card(input.name || `${role} · 名称尚未获取`, label,
      input.ready ? 'ready' : input.paused ? 'warning' : 'unknown', input.address, details);
    if(input.phase)node.append(text('p',`连接阶段：${input.phase} · ${(input.phase_age_ms/1000).toFixed(1)} 秒 · 本轮 ${(input.attempt_age_ms/1000).toFixed(1)} 秒${input.last_error?` · 最近错误 ${input.last_error}`:''}`,'details'));
    node.append(text('p',input.report_seen?`本次连接收到过输入，最近报告 ${(input.last_report_age_ms/1000).toFixed(1)} 秒前`:
      '本次连接尚未收到真实输入；按键／移动后观察报告数，不因正常空闲自动重连','details'));
    node.append(actionButton('重连原设备（不重配）',()=>command('retry',{slot:input.slot}),
      Boolean(input.saved && !managementBlock(data) && input.pause_reason!==2 && input.pause_reason!==3),
      managementBlock(data)||'密钥／报告格式异常需要明确处理'));
    deleteButton(node, input.name || role, input.address, {slot: input.record_slot}, data, false, Boolean(input.record_slot));
    return node;
  }));
  byId('computers').replaceChildren(...[1, 2, 3, 4, 5, 6, 7].map(slot => {
    if (!current || data.storage_fault) {
      const node = card(`PC${slot}`, '状态未知', 'unknown', '', '等待有效的 Pico 状态');
      node.append(actionButton('删除此电脑', () => {}, false, '电脑状态未知，不能删除'));
      node.append(text('p', '电脑状态未知，不能删除', 'control-note'));
      return node;
    }
    const pc = data.computers[slot - 1];
    const label = !pc.saved ? '未录入' : pc.suspended ? '电脑已暂停 HID' : pc.ready ? 'HID 就绪' : pc.connected ? '已连接，未就绪' : '离线';
    const selected = slot === data.selected_pc;
    const details = `${selected ? '当前选中 · ' : ''}${pc.saved ? '有配对槽 · ' : ''}${pc.encrypted ? '已加密 · ' : ''}${pc.interval_ms == null ? '' : `${pc.interval_ms} ms · `}${pc.ready && selected && !data.switching ? '正在接收键鼠输入' : '未向此电脑发送普通输入'}`;
    const node = card(`PC${slot}`, label, pc.ready ? 'ready' : 'unknown', pc.address, details, selected);
    deleteButton(node, `PC${slot}`, pc.address, {slot}, data, true, pc.saved);
    return node;
  }));
  const trialPhase = {1: '等待旧设备连接结束', 2: '验证新设备', 4: '恢复旧设备'}[data.trial_phase] || '处理录入';
  byId('management').textContent = data.management_open ? `已实体授权，剩余 ${Math.ceil(data.remaining_ms / 1000)} 秒${data.auto_enrollment ? ' · 自动录入开启' : ' · 手动管理'}${data.scan_waiting ? ' · 扫描排队中，将自动开始' : data.scanning ? ' · 扫描中' : ''}${data.trial ? ` · ${trialPhase}（${data.trial_role === 2 ? '鼠标' : '键盘'} ${data.trial_address}）` : ''}` :
    '管理未授权：长按 ESP32 BOOT 3 秒后松开；双击可开关热点。';
  byId('operation').textContent = data.operation || '';
  byId('input-events').replaceChildren(...(data.input_events||[]).slice().reverse().map(event=>
    text('p',`${(event.age_ms/1000).toFixed(1)} 秒前 · 输入 ${event.slot} · ${event.phase} · ${event.event} · ${event.detail}`,'details')));
  byId('scan-classic').disabled = byId('scan-ble').disabled = !data.management_open || data.trial ||
    data.scanning || data.input_storage_fault || data.deletion_pending;
  byId('cancel').disabled = !data.management_open;
  byId('manual').disabled = !data.management_open || data.trial || data.deletion_pending;
  byId('records').replaceChildren(...(data.records || []).map(record => {
    const node = text('article', '', 'record');
    node.append(text('strong', `输入槽 ${record.slot} · ${record.saved ? (record.name || `${record.role === 1 ? '键盘' : '鼠标'}（名称尚未获取）`) : '未保存设备'}${record.active ? ' · 当前使用' : ''}`));
    if (record.saved) {
      node.append(text('p', `${record.transport} · ${record.address} · 录入顺序 ${record.order}`));
      const reason = managementBlock(data);
      node.append(actionButton('启用此设备', () => command('activate', {slot: record.slot, address: record.address}), !reason, reason));
      node.append(actionButton('设置名称', () => {
        const name = prompt('输入设备名称（最多 63 字节）：', record.name || '');
        if (name?.trim()) command('rename', {slot: record.slot, address: record.address, name: name.trim()});
      }, !reason, reason));
    }
    deleteButton(node, record.name || `输入槽 ${record.slot}`, record.address, {slot: record.slot}, data, false, record.saved);
    return node;
  }));
  const sortedCandidates = [...(data.candidates || [])].sort((a,b) => Number(b.hid) - Number(a.hid));
  byId('candidates').replaceChildren(...sortedCandidates.map(candidate => {
    const node = text('article', '', 'record');
    node.append(text('strong', candidate.name || '未广播名称'), text('p', `${candidate.transport} · ${candidate.address}${candidate.hid ? ' · HID 候选' : ' · 类型待验证'}`));
    for (const [slot, role] of [[1, '键盘'], [2, '鼠标']]) node.append(actionButton(`作为${role}录入`, () => {
      const full = (data.records || []).filter(r => r.saved).length >= 5;
      if (confirm(`将 ${candidate.name || candidate.address} 作为${role}？${full ? '槽已满，验证成功后淘汰最老记录。' : '原设备记录保留，失败恢复原设备。'}`))
        command('connect', {slot, candidate: candidate.id, address: candidate.address, replace: full});
    }, data.management_open && !data.scanning && !data.trial && !data.input_storage_fault && !data.deletion_pending));
    return node;
  }));
  const authKey = data.management_open && data.auth ?
    `${data.token}:${data.auth_slot}:${data.auth}:${data.auth_number ?? ''}:${data.trial_address ?? ''}` : '';
  if (authKey !== authSignature) {
    authSignature = authKey;
    byId('auth').replaceChildren();
    if (authKey) {
      byId('auth').append(text('p', data.auth === 3 ? '在新键盘本体输入 123456，然后按 Enter。' :
        data.auth === 1 ? `核对设备显示的数字：${String(data.auth_number).padStart(6, '0')}` : '设备要求输入六位配对码。'));
      if (data.auth === 1) for (const [value, label] of [['yes', '数字一致'], ['no', '拒绝']])
        byId('auth').append(actionButton(label, () => command('confirm', {slot: data.auth_slot, value}), true));
      if (data.auth === 2) {
        const code = text('input', ''); code.type = 'password'; code.inputMode = 'numeric'; code.maxLength = 6;
        byId('auth').append(code, actionButton('提交配对码', () => {
          if (/^[0-9]{6}$/.test(code.value)) command('passkey', {slot: data.auth_slot, value: code.value});
          else byId('command-result').textContent = '请输入六位数字。';
          code.value = '';
        }, true));
      }
    }
  }
  byId('network').textContent = `热点 ${data.ssid} · ${data.ip} · 管理客户端 ${data.clients}/2 · 运行 ${(data.uptime_ms / 60000).toFixed(1)} 分钟`;
  byId('versions').textContent = `ESP32 ${data.firmware} · Pico ${current ? data.pico_firmware : '未知'} · 鼠标缓冲 ${current ? (data.mouse_buffer ? '开启' : '关闭') : '未知'} · UART 队列溢出 ${data.uart_overflows}`;
  byId('updated').textContent = `页面更新时间 ${new Date().toLocaleTimeString()} · 每 2 秒读取，不代表绝对输入延迟`;
}
async function refresh() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 2500);
  try {
    const response = await fetch('/api/status', {cache: 'no-store', signal: controller.signal});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (error) {
    unknown(`读取状态失败：${error.message}。请确认仍连接 ESP32 热点；稍后自动重试。`);
  } finally {
    clearTimeout(timeout);
    setTimeout(refresh, 2000);
  }
}
unknown('正在连接 ESP32 状态接口…');
byId('scan-classic').addEventListener('click', () => command('scan', {transport: 'Classic'}));
byId('manual').addEventListener('click', () => command('manual'));
byId('scan-ble').addEventListener('click', () => command('scan', {transport: 'BLE'}));
byId('cancel').addEventListener('click', () => command('cancel'));
refresh();
