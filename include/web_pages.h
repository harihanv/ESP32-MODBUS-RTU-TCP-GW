#pragma once
#include <Arduino.h>

const char LOGIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>Gateway Login</title>
<style>
*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:#e2e8f0;display:grid;place-items:center;min-height:100vh;margin:0;padding:20px}
.card{width:min(100%,380px);background:white;padding:28px;border-radius:10px;box-shadow:0 8px 28px #0f172a24}
h1{font-size:22px;margin:0 0 8px}p{color:#64748b;margin:0 0 18px}label{display:block;font-weight:600;margin:12px 0 5px}
input{width:100%;padding:11px;border:1px solid #cbd5e1;border-radius:5px}button{width:100%;margin-top:18px;padding:11px;border:0;border-radius:5px;background:#2563eb;color:white;font-weight:bold;cursor:pointer}
.error{color:#b91c1c;min-height:20px;margin-top:12px}
</style></head><body><main class="card"><h1 id="login-title">Modbus Gateway Login</h1><p id="login-description">Sign in to view and configure this device.</p>
<form id="login"><label for="username">Username</label><input id="username" value="admin" autocomplete="username" readonly>
<label for="password">Password</label><input id="password" name="password" type="password" autocomplete="current-password" minlength="1" maxlength="64" required>
<div id="confirm-row" hidden><label for="confirm-password">Confirm password</label><input id="confirm-password" type="password" autocomplete="new-password" maxlength="64"></div>
<button id="submit" type="submit" disabled>Checking account setup...</button><div id="error" class="error" role="alert"></div></form></main>
<script>
let authConfigured=null;
async function loadAuthStatus(){
try{
const response=await fetch('/api/auth/status',{cache:'no-store'});
if(!response.ok)throw new Error('HTTP '+response.status);
const status=await response.json();
if(typeof status.configured!=='boolean')throw new Error('Invalid account status response');
authConfigured=status.configured===true;
if(!authConfigured){
document.getElementById('login-title').textContent='Create Gateway Account';
document.getElementById('login-description').textContent='Create the first administrator account.';
document.getElementById('confirm-row').hidden=false;
document.getElementById('submit').textContent='Create account';
document.getElementById('password').autocomplete='new-password';
}else document.getElementById('submit').textContent='Sign in';
document.getElementById('submit').disabled=false;
}catch(error){
authConfigured=null;
document.getElementById('error').textContent='Could not check account setup status. Reload this page and try again.';
document.getElementById('submit').textContent='Account status unavailable';
document.getElementById('submit').disabled=true;
}
}
async function readAuthResponse(response){
const body=await response.text();
try{return JSON.parse(body);}
catch(error){throw new Error(`Gateway returned an invalid response (HTTP ${response.status}): ${body.slice(0,160)||'empty response'}`);}
}
document.getElementById('login').onsubmit=async e=>{
e.preventDefault();const button=document.getElementById('submit'),error=document.getElementById('error');
if(authConfigured===null){error.textContent='Account setup status is unavailable. Reload the page before continuing.';return;}
button.disabled=true;error.textContent='';
try{
const accountPassword=document.getElementById('password').value;
if(!authConfigured&&accountPassword!==document.getElementById('confirm-password').value)throw new Error('Passwords do not match.');
const response=await fetch(authConfigured?'/api/login':'/api/auth/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:accountPassword})});
const result=await readAuthResponse(response);
if(!response.ok||!result.success)throw new Error(result.error||(authConfigured?'Login failed.':'Account setup failed.'));
location.replace('/');
}catch(err){error.textContent=err.message||'Could not contact the gateway.';button.disabled=false;}
};
loadAuthStatus();
</script></body></html>
)rawliteral";

const char WIFI_SETUP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>Gateway WiFi Setup</title>
<style>
body{font-family:sans-serif;background:#4a5568;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}
.card{background:white;padding:30px;border-radius:8px;width:320px;box-shadow:0 4px 6px rgba(0,0,0,0.3);}
input{width:100%;padding:10px;margin:8px 0 16px;box-sizing:border-box;}
button{width:100%;padding:10px;background:#3182ce;color:white;border:none;border-radius:4px;font-weight:bold;cursor:pointer;}
.toast{position:fixed;z-index:1000;top:20px;left:50%;transform:translate(-50%,-16px);max-width:min(92vw,520px);padding:12px 18px;border-radius:8px;background:#1e293b;color:white;box-shadow:0 8px 24px #0004;opacity:0;visibility:hidden;transition:opacity .2s,transform .2s,visibility .2s;overflow-wrap:anywhere}
.toast.show{opacity:1;visibility:visible;transform:translate(-50%,0)}.toast.error{background:#b91c1c}.toast.success{background:#166534}
.reboot-overlay{position:fixed;inset:0;z-index:2000;display:none;align-items:center;justify-content:center;padding:20px;background:#0f172acc}
.reboot-overlay.show{display:flex}.reboot-dialog{width:min(100%,420px);padding:26px;background:white;border-radius:12px;box-shadow:0 18px 50px #0005;text-align:center}
.reboot-dialog h2{margin-top:0}.reboot-dialog button{display:none}
.reboot-dialog a{display:inline-block;margin-top:12px;color:#2563eb}
.connection-status{margin:16px 0 0;padding:12px;border-radius:6px;background:#edf2f7;color:#334155;font-size:14px}
.connection-status[data-state="success"]{background:#dcfce7;color:#166534}
.connection-status[data-state="warning"]{background:#fef3c7;color:#92400e}
.connection-status[data-state="error"]{background:#fee2e2;color:#991b1b}
.connection-status[data-state="connecting"]::before{content:"";display:inline-block;width:12px;height:12px;margin-right:8px;border:2px solid #94a3b8;border-top-color:#2563eb;border-radius:50%;vertical-align:-2px;animation:spin .8s linear infinite}
#wifi-setup-card{width:min(100% - 32px,420px);padding:24px}
#wifi-setup-card>p{font-size:14px;line-height:1.45}
.network-picker{display:flex;gap:8px;align-items:end;margin:14px 0 4px}
.network-picker label{flex:1;min-width:0;margin:0}
.network-picker select{height:42px}
#scan-wifi{width:auto;min-width:76px;height:42px;padding:0 14px;margin:0;white-space:nowrap}
#wifi-connection-status{display:none;margin:10px 0;padding:9px 11px}
#wifi-connection-status.visible{display:block}
@keyframes spin{to{transform:rotate(360deg)}}
</style></head><body>
<div id="wifi-setup-card" class="card">
<h2>WiFi Setup</h2>
<p>Setup address: <strong>http://esp32-modbus-gw.local</strong></p>
<p>Choose a nearby network or enter its name manually.</p>
<div class="network-picker"><label for="available-networks">Nearby Wi-Fi
<select id="available-networks"><option value="">Select a network (optional)</option></select></label>
<button id="scan-wifi" type="button">Scan</button></div>
<p id="wifi-connection-status" class="connection-status" data-state="info" role="status" aria-live="polite"></p>
<form id="f">
<label for="s">Network name (SSID)</label><input type="text" id="s" maxlength="32" autocomplete="off" required>
<label for="p">Password (leave blank for an open network)</label><input type="password" id="p" maxlength="64" autocomplete="new-password">
<button id="save" type="submit">Save & Connect</button>
</form>
</div>
<div id="toast" class="toast" role="status" aria-live="polite"></div>
<div id="wifi-reboot-overlay" class="reboot-overlay" role="alertdialog" aria-modal="true" aria-labelledby="wifi-reboot-title">
<div class="reboot-dialog"><h2 id="wifi-reboot-title">Connecting to Wi-Fi</h2>
<p id="wifi-reboot-message">Credentials were saved. Waiting for the gateway to restart...</p>
<button id="wifi-reboot-retry" type="button" style="display:none">Check again</button>
<a id="wifi-reboot-link" href="http://esp32-modbus-gw.local/">http://esp32-modbus-gw.local</a></div></div>
<script>
let toastTimer;
let wifiScanRunning=false;
let wifiTransitionTimer=null;
let wifiTransitionDeadline=0;
let wifiTransitionConnected=false;
let wifiTransitionTarget='';
const wifiStatus=document.getElementById('wifi-connection-status');
function showToast(message,type=''){
const toast=document.getElementById('toast');toast.textContent=message;toast.className='toast show '+type;
clearTimeout(toastTimer);if(type!=='success')toastTimer=setTimeout(()=>toast.classList.remove('show'),5000);
}
function setWifiStatus(message,state='info'){
wifiStatus.textContent=message;wifiStatus.dataset.state=state;
wifiStatus.classList.add('visible');
}
function setTransitionMessage(message){
document.getElementById('wifi-reboot-message').textContent=message;
setWifiStatus(message,'connecting');
}
function startWifiTransition(ssid){
wifiTransitionTarget=ssid;
wifiTransitionDeadline=Date.now()+90000;
wifiTransitionConnected=false;
document.getElementById('wifi-reboot-overlay').classList.add('show');
document.getElementById('wifi-reboot-title').textContent='Connecting to Wi-Fi';
document.getElementById('wifi-reboot-retry').style.display='none';
setTransitionMessage(`Credentials for "${ssid}" are saved. Waiting for the gateway...`);
clearInterval(wifiTransitionTimer);
wifiTransitionTimer=setInterval(pollWifiTransition,1800);
setTimeout(pollWifiTransition,900);
}
async function pollWifiTransition(){
const now=Date.now();
const message=document.getElementById('wifi-reboot-message');
try{
const response=await fetch('/api/health?transition='+now,{cache:'no-store'});
if(response.ok){
const result=await response.json();
if(result.wifi_connected){
clearInterval(wifiTransitionTimer);wifiTransitionTimer=null;
wifiTransitionConnected=true;
document.getElementById('wifi-reboot-title').textContent='Wi-Fi connected';
setWifiStatus(`Connected to "${wifiTransitionTarget}". Reconnect your phone or computer to that Wi-Fi network, then open the dashboard below. The setup network will remain available briefly.`, 'success');
message.textContent=`Connected to "${wifiTransitionTarget}". The setup network remains available briefly so this status can finish displaying. Reconnect your phone or computer to "${wifiTransitionTarget}", then open the dashboard below.`;
const dashboardUrl=result.dashboard_url;
const dashboardLink=document.getElementById('wifi-reboot-link');
dashboardLink.href=dashboardUrl;dashboardLink.textContent=dashboardUrl;
const retryButton=document.getElementById('wifi-reboot-retry');
retryButton.style.display='inline-block';retryButton.textContent='Open dashboard after joining Wi-Fi';
return;
}
if(result.setup_mode){
setTransitionMessage(`Setup network is available. The gateway is retrying "${wifiTransitionTarget}"...`);
}else{
setTransitionMessage('The gateway has restarted. Checking its Wi-Fi connection...');
}
}else if(response.status>=500){
setTransitionMessage('The gateway is restarting. Waiting for its web server...');
}
}catch(error){
setTransitionMessage('The gateway is reconnecting. This page will keep checking automatically...');
}
if(now>=wifiTransitionDeadline){
clearInterval(wifiTransitionTimer);wifiTransitionTimer=null;
document.getElementById('wifi-reboot-title').textContent='Still trying to connect';
message.textContent=`The gateway has not confirmed a connection to "${wifiTransitionTarget}" yet. Keep this page open on the setup network, or select another network and try again.`;
setWifiStatus(message.textContent,'warning');
const retryButton=document.getElementById('wifi-reboot-retry');
retryButton.style.display='inline-block';retryButton.textContent='Check again';
}
}
document.getElementById('wifi-reboot-retry').onclick=()=>{
if(wifiTransitionConnected){
location.assign(document.getElementById('wifi-reboot-link').href);
return;
}
if(wifiTransitionTimer){pollWifiTransition();return;}
startWifiTransition(wifiTransitionTarget);
};
document.getElementById('available-networks').onchange=e=>{
if(e.target.value)document.getElementById('s').value=e.target.value;
};
async function scanWifiNetworks(button,list,status,ssidInput,onStatus){
if(wifiScanRunning)return;
wifiScanRunning=true;
list.onchange=()=>{if(list.value)ssidInput.value=list.value;};
const deadline=Date.now()+30000;
button.disabled=true;button.textContent='...';
list.replaceChildren(new Option('Scanning...',''));
if(onStatus)setWifiStatus('Scanning for nearby Wi-Fi networks...','connecting');
else{status.textContent='Scanning for nearby Wi-Fi networks...';}
try{
let result;
do{
const response=await fetch('/api/wifi/scan?poll='+Date.now(),{cache:'no-store'});
if(response.status===401)throw new Error('Sign in again to scan for networks.');
result=await response.json();
if(!response.ok)throw new Error(result.error||'Wi-Fi scan failed.');
if(result.scanning){
if(Date.now()>=deadline)throw new Error('Scan timed out. Enter the network name manually.');
await new Promise(resolve=>setTimeout(resolve,800));
}
}while(result.scanning);
list.replaceChildren(new Option('Select a network (optional)',''));
result.networks.forEach(network=>{
const signal=network.rssi>=-60?'Strong':network.rssi>=-75?'Good':'Weak';
list.add(new Option(`${network.ssid} · ${signal}${network.secure?' · secured':''}`,network.ssid));
});
if(result.networks.length===0){
list.replaceChildren(new Option('No networks found',''));
const message='No networks found. Check that the network is 2.4 GHz, or enter its name manually.';
if(onStatus)setWifiStatus(message,'warning');else status.textContent=message;
}else{
const message=`${result.networks.length} network${result.networks.length===1?'':'s'} found. Select one or enter the SSID manually.`;
if(onStatus)setWifiStatus(message,'success');else status.textContent=message;
}
}catch(error){
list.replaceChildren(new Option('Scan unavailable',''));
const message=error.message||'Unable to scan for networks. Enter an SSID manually.';
if(onStatus)setWifiStatus(message,'error');else status.textContent=message;
}finally{
wifiScanRunning=false;button.disabled=false;button.textContent='Scan';
}
}
document.getElementById('scan-wifi').onclick=()=>scanWifiNetworks(
document.getElementById('scan-wifi'),
document.getElementById('available-networks'),
null,document.getElementById('s'),true);
document.getElementById('f').onsubmit=async e=>{
e.preventDefault();
const button=document.getElementById('save');
button.disabled=true; showToast('Saving Wi-Fi settings...');
try{
const response=await fetch('/api/wifi/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({
ssid:document.getElementById('s').value,password:document.getElementById('p').value
})});
const result=await response.json();
if(!response.ok||!result.success) throw new Error(result.error||'Could not save Wi-Fi settings.');
startWifiTransition(document.getElementById('s').value);
}catch(error){
showToast(error.message||'Unable to reach the gateway.','error');
setWifiStatus(error.message||'Unable to save Wi-Fi settings.','error');
button.disabled=false;
}
};
</script></body></html>
)rawliteral";

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>ESP32 Modbus Gateway</title>
<style>
*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:#f1f5f9;margin:0;color:#1e293b}
.layout{display:flex;min-height:100vh}.sidebar{width:230px;background:#0f172a;color:white;padding:22px 14px;flex-shrink:0}
.brand{font-size:18px;font-weight:bold;padding:0 10px 20px}.nav button{display:block;width:100%;text-align:left;padding:12px 10px;margin:4px 0;background:transparent;color:#cbd5e1;border:0;border-radius:6px;cursor:pointer}
.nav button.active,.nav button:hover{background:#1e293b;color:white}.main{flex:1;padding:0 28px 28px;max-width:1200px}
.top{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin:0 -28px 20px;padding:22px 56px;background:#0f172a;color:white}.top h1{font-size:18px;line-height:normal;margin:0 0 8px}.top .muted{color:#cbd5e1}.panel{display:none}.panel.active{display:block}
.card{background:#fff;padding:22px;border-radius:10px;margin-bottom:16px;box-shadow:0 1px 4px #0f172a12}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px}.stat{background:#f8fafc;padding:13px;border-left:4px solid #3b82f6;border-radius:5px}
.state-flow{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}.state-step{padding:12px;background:#f1f5f9;border:1px solid #cbd5e1;border-radius:6px;color:#64748b}.state-step.active{background:#dbeafe;border-color:#2563eb;color:#1e3a8a;font-weight:bold}
.state-current{display:inline-flex;align-items:center;gap:9px;margin:2px 0 14px;padding:9px 12px;border-radius:999px;background:#fef3c7;color:#92400e;font-size:14px;font-weight:bold}
.state-current::before{content:"";width:10px;height:10px;border-radius:50%;background:currentColor}
.state-current.configuration{background:#dbeafe;color:#1e40af}.state-current.data-transfer{background:#dcfce7;color:#166534}
.stat-val{font-size:18px;font-weight:bold;margin-top:5px}.form-grid{display:grid;grid-template-columns:repeat(2,minmax(160px,1fr));gap:14px}
.network-picker{display:flex;gap:8px;align-items:end;margin:8px 0 4px}
.network-picker label{flex:1;min-width:0;margin:0}
.network-picker select{height:42px}
#network-scan-button{width:auto;height:42px;padding:0 16px;margin:0;white-space:nowrap}
#network-scan-status{min-height:0;margin:4px 0}
label{display:block;font-size:14px;font-weight:600;margin:8px 0 4px}input,select{width:100%;padding:10px;border:1px solid #cbd5e1;border-radius:5px}
button.action{padding:10px 16px;background:#2563eb;color:white;border:0;border-radius:5px;cursor:pointer;margin:12px 5px 0 0}
button.danger{background:#b91c1c}.muted{color:#64748b;font-size:14px}.logs{background:#0f172a;color:#cbd5e1;padding:12px;border-radius:6px;min-height:240px;max-height:420px;overflow:auto;white-space:pre-wrap;font:12px monospace}
.toast{position:fixed;z-index:1000;top:20px;left:50%;transform:translate(-50%,-16px);max-width:min(92vw,520px);padding:12px 18px;border-radius:8px;background:#1e293b;color:white;box-shadow:0 8px 24px #0004;opacity:0;visibility:hidden;transition:opacity .2s,transform .2s,visibility .2s;overflow-wrap:anywhere}
.toast.show{opacity:1;visibility:visible;transform:translate(-50%,0)}.toast.error{background:#b91c1c}.toast.success{background:#166534}
@media(max-width:700px){.layout{display:block}.sidebar{width:100%;padding:12px}.brand{padding-bottom:8px}.nav{display:flex;overflow:auto}.nav button{white-space:nowrap;width:auto}.main{padding:0 16px 16px}.top{margin:0 -16px 20px;padding:14px 16px}.form-grid{grid-template-columns:1fr}}
</style></head><body>
<div class="layout">
<aside class="sidebar"><div class="brand">Modbus Gateway</div><nav class="nav">
<button class="active" data-panel="overview">Dashboard</button>
<button data-panel="network">Network Settings</button>
<button data-panel="serial">Serial / RTU Settings</button>
<button data-panel="modbus-tcp">Modbus TCP Settings</button>
<button data-panel="diagnostic">RTU Diagnostics</button>
<button data-panel="ota">OTA Update</button>
<button data-panel="logs">System Logs</button>
<button data-panel="system">System Settings</button>
</nav></aside>
<main class="main">
<div id="toast" class="toast" role="status" aria-live="polite"></div>
<header class="top"><div><h1 id="page-title">Dashboard</h1><span class="muted">Firmware <span id="ver">-</span> | Gateway state: <strong id="state">INITIALIZATION</strong> | Activity: <strong id="comm-mode">IDLE</strong> | IP: <strong id="ip">-</strong></span></div>
<button class="action danger" onclick="logout()">Log out</button></header>
<section class="panel active" id="overview">
<div class="card"><h2>Gateway operating state</h2>
<div id="current-state" class="state-current" role="status" aria-live="polite"><span id="current-state-name">INITIALIZATION</span></div>
<div class="state-flow"><div id="step-initialization" class="state-step">1. Initialization</div><div id="step-configuration" class="state-step">2. Configuration</div><div id="step-data-transfer" class="state-step">3. Data Transfer</div></div>
<p id="state-description" class="muted">Initializing Wi-Fi, RTU, and TCP bridge.</p></div>
<div class="card"><h2>Device status</h2>
<div class="grid">
<div class="stat">WiFi<div id="w-stat" class="stat-val">-</div></div>
<div class="stat">Uptime<div id="upt" class="stat-val">-</div></div>
<div class="stat">Requests<div id="reqs" class="stat-val">-</div></div>
<div class="stat">Responses<div id="responses" class="stat-val">-</div></div>
<div class="stat">Errors<div id="errs" class="stat-val">-</div></div>
</div></div>
</section>
<section class="panel" id="network"><div class="card"><h2>Network Settings</h2><p class="muted">Enter your Wi-Fi credentials and choose DHCP or static IPv4. DHCP is selected by default. Settings are saved before the device reboots to connect.</p>
<form id="network-form"><div class="network-picker"><label for="network-scan-results">Nearby Wi-Fi
<select id="network-scan-results"><option value="">Select a network (optional)</option></select></label>
<button class="action" id="network-scan-button" type="button">Scan</button></div>
<p id="network-scan-status" class="muted" role="status" aria-live="polite"></p>
<label for="ssid">Wi-Fi SSID</label><input id="ssid" maxlength="32" required>
<label for="password">Wi-Fi password</label><input id="password" type="password" maxlength="64" autocomplete="new-password">
<label for="network-mode">IP address assignment</label><select id="network-mode">
<option value="dhcp">DHCP (automatic)</option><option value="static">Static</option></select>
<div id="network-static-fields" hidden>
<label for="network-ip">Static IP address</label><input id="network-ip" type="text" placeholder="192.168.1.100">
<label for="network-gateway">Gateway</label><input id="network-gateway" type="text" placeholder="192.168.1.1">
<label for="network-subnet">Subnet mask</label><input id="network-subnet" type="text" placeholder="255.255.255.0">
<label for="network-dns">DNS server</label><input id="network-dns" type="text" placeholder="8.8.8.8">
</div>
<button class="action" type="submit">Save & Connect</button></form>
</div></section>
<section class="panel" id="serial"><div class="card"><h2>Serial / RTU Settings</h2><p class="muted">Settings are applied after saving and rebooting.</p>
<p class="muted">Viewing or editing these settings does not pause Modbus traffic. RTU settings take effect after the gateway reboots.</p>
<form id="serial-form"><div class="form-grid">
<div><label for="baud">Baud rate</label><select id="baud"><option>1200</option><option>2400</option><option>4800</option><option>9600</option><option>19200</option><option>38400</option><option>57600</option><option>115200</option></select></div>
<div><label for="data-bits">Data bits</label><select id="data-bits"><option value="8">8</option><option value="7">7</option></select></div>
<div><label for="parity">Parity</label><select id="parity"><option value="N">None</option><option value="E">Even</option><option value="O">Odd</option></select></div>
<div><label for="stop-bits">Stop bits</label><select id="stop-bits"><option value="1">1</option><option value="2">2</option></select></div>
<div><label for="rtu-timeout-ms">RTU timeout (ms)</label><input id="rtu-timeout-ms" type="number" min="50" max="60000" step="10" value="500" required></div>
</div><button class="action" type="submit">Save Serial Settings</button></form></div></section>
<section class="panel" id="modbus-tcp"><div class="card"><h2>Modbus TCP Settings</h2>
<p class="muted">Configure the TCP listening port and response timeout. Changes take effect after the gateway reboots.</p>
<form id="tcp-form"><div class="form-grid">
<div><label for="tcp-port">Listening port</label><input id="tcp-port" type="number" min="1" max="65535" required></div>
<div><label for="tcp-timeout-ms">Modbus TCP timeout (ms)</label><input id="tcp-timeout-ms" type="number" min="100" max="60000" step="10" value="1000" required></div>
</div><button class="action" type="submit">Save Modbus TCP Settings</button></form></div></section>
<section class="panel" id="diagnostic"><div class="card"><h2>Modbus RTU Diagnostics</h2>
<p class="muted">Read holding or input registers from an RTU unit. The gateway builds the Modbus request and adds the RTU CRC automatically.</p>
<form id="rtu-diagnostic-form"><label for="diag-unit">Unit ID (1-247)</label><input id="diag-unit" type="number" min="1" max="247" value="1" required>
<label for="diag-register-type">Register type</label><select id="diag-register-type"><option value="3">Holding Registers (03)</option><option value="4">Input Registers (04)</option></select>
<label for="diag-address">Starting address (0-65535)</label><input id="diag-address" type="number" min="0" max="65535" value="1" required>
<label for="diag-count">Number of registers (1-125)</label><input id="diag-count" type="number" min="1" max="125" value="4" required>
<button id="diag-send" class="action" type="submit">Send RTU Command</button></form>
<h3>Raw exchange</h3><pre class="logs" id="diag-result">No diagnostic command sent. Full RTU frames, including CRC bytes, will appear here.</pre></div></section>
<section class="panel" id="ota"><div class="card"><h2>Firmware Update</h2>
<p class="muted">Upload a firmware .bin file built for this gateway. The image is checked before the device switches to it. Configuration and Wi-Fi settings are preserved.</p>
<p id="ota-capacity" class="muted">Checking available update slot...</p>
<form id="ota-form"><label for="ota-file">Firmware image (.bin)</label><input id="ota-file" type="file" accept=".bin,application/octet-stream" required>
<progress id="ota-progress" max="100" value="0" style="width:100%;margin-top:12px;display:none"></progress>
<div id="ota-message" class="muted" role="status" aria-live="polite"></div>
<button id="ota-upload" class="action" type="submit">Upload Firmware</button></form></div></section>
<section class="panel" id="logs"><div class="card"><h2>System Logs</h2><pre class="logs" id="logbox"></pre><button class="action" onclick="clearLogs()">Clear Logs</button></div></section>
<section class="panel" id="system">
<div class="card"><h2>Login Credentials</h2><p class="muted">Username is fixed to admin. The password is stored directly in this device's non-volatile storage. Changing it revokes the current session; sign in again with the new password.</p>
<form id="auth-form"><label>Username</label><input value="admin" readonly>
<label for="auth-password">New password (1-64 characters)</label><input id="auth-password" type="password" minlength="1" maxlength="64" autocomplete="new-password" required>
<button class="action" type="submit">Update Login Credentials</button></form></div>
<div class="card"><h2>Logging</h2><p class="muted">Configure serial-console and System Logs verbosity independently. Debug includes Modbus frame details. These settings are saved across reboots.</p>
<form id="logging-form"><label for="serial-log-level">Serial console</label><select id="serial-log-level">
<option value="0">Errors only</option><option value="1">Warnings and errors</option>
<option value="2">Info, warnings, and errors</option><option value="3">Debug (all messages)</option></select>
<label for="web-log-level">System Logs</label><select id="web-log-level">
<option value="0">Errors only</option><option value="1">Warnings and errors</option>
<option value="2">Info, warnings, and errors</option><option value="3">Debug (all messages)</option></select>
<button class="action" type="submit">Save Logging Settings</button></form></div>
<div class="card"><h2>System actions</h2><p>Factory reset clears Wi-Fi and login credentials and restores serial and TCP defaults. You will create a new login account after reset.</p>
<button class="action danger" onclick="factoryReset()">Factory Reset</button><button class="action" onclick="rebootDevice()">Reboot</button></div></section>
</main></div>
<script>
const titles={overview:'Dashboard',network:'Network Settings',serial:'Serial / RTU Settings','modbus-tcp':'Modbus TCP Settings',diagnostic:'RTU Diagnostics',ota:'OTA Update',logs:'System Logs',system:'System Settings'};
const baudSelect=document.getElementById('baud'),paritySelect=document.getElementById('parity');
let toastTimer,activePanel='overview',panelChangeInProgress=false;
let networkScanBusy=false;
function showToast(message,type=''){
const toast=document.getElementById('toast');toast.textContent=message;toast.className='toast show '+type;
clearTimeout(toastTimer);if(type!=='success')toastTimer=setTimeout(()=>toast.classList.remove('show'),5000);
}
function requireLogin(response){
if(response.status===401){showToast('Session expired. Sign in again to continue.','error');return false;}
return true;
}
async function scanDashboardNetworks(){
if(networkScanBusy)return;
networkScanBusy=true;
const button=document.getElementById('network-scan-button');
const list=document.getElementById('network-scan-results');
const status=document.getElementById('network-scan-status');
const ssid=document.getElementById('ssid');
button.disabled=true;button.textContent='...';status.textContent='Scanning...';
list.replaceChildren(new Option('Scanning...',''));
const deadline=Date.now()+30000;
try{
let result;
do{
const response=await fetch('/api/wifi/scan?poll='+Date.now(),{cache:'no-store'});
if(!requireLogin(response))throw new Error('Session expired. Sign in again.');
result=await response.json();
if(!response.ok)throw new Error(result.error||'Wi-Fi scan failed.');
if(result.scanning){
if(Date.now()>=deadline)throw new Error('Scan timed out. Enter the SSID manually.');
await new Promise(resolve=>setTimeout(resolve,800));
}
}while(result.scanning);
list.replaceChildren(new Option('Select a network (optional)',''));
result.networks.forEach(network=>{
const signal=network.rssi>=-60?'Strong':network.rssi>=-75?'Good':'Weak';
list.add(new Option(`${network.ssid} · ${signal}${network.secure?' · secured':''}`,network.ssid));
});
if(result.networks.length===0){
list.replaceChildren(new Option('No networks found',''));
status.textContent='No networks found. Enter the SSID manually.';
}else{
status.textContent=`${result.networks.length} network${result.networks.length===1?'':'s'} found.`;
}
}catch(error){
list.replaceChildren(new Option('Scan unavailable',''));
status.textContent=error.message||'Unable to scan. Enter the SSID manually.';
}finally{
networkScanBusy=false;button.disabled=false;button.textContent='Scan';
}
}
document.getElementById('network-scan-results').onchange=event=>{
if(event.target.value)document.getElementById('ssid').value=event.target.value;
};
document.getElementById('network-scan-button').onclick=scanDashboardNetworks;
document.querySelectorAll('.nav button').forEach(button=>button.onclick=async()=>{
if(panelChangeInProgress||button.dataset.panel===activePanel)return;
panelChangeInProgress=true;
const nextPanel=button.dataset.panel;
try{
document.querySelectorAll('.nav button').forEach(b=>b.classList.toggle('active',b===button));
document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('active',p.id===nextPanel));
document.getElementById('page-title').textContent=titles[nextPanel];
activePanel=nextPanel;
}finally{panelChangeInProgress=false;}
});
async function pollStatus(){
try{
let r=await fetch('/api/status');if(!requireLogin(r)||!r.ok)return;let d=await r.json();
document.getElementById('ver').textContent=d.version;
document.getElementById('w-stat').textContent=d.wifi_connected?'Connected':'AP Mode';
document.getElementById('state').textContent=d.state;
const stateLabels={INITIALIZATION:'Initialization',CONFIGURATION:'Configuration',DATA_TRANSFER:'Data Transfer'};
document.getElementById('current-state-name').textContent=stateLabels[d.state]||'Unknown';
document.getElementById('current-state').className='state-current '+({'CONFIGURATION':'configuration','DATA_TRANSFER':'data-transfer'}[d.state]||'');
document.getElementById('comm-mode').textContent=d.communication_mode||'-';
document.getElementById('ip').textContent=d.ip;
const stateSteps={INITIALIZATION:'step-initialization',CONFIGURATION:'step-configuration',DATA_TRANSFER:'step-data-transfer'};
Object.values(stateSteps).forEach(id=>document.getElementById(id).classList.remove('active'));
if(stateSteps[d.state])document.getElementById(stateSteps[d.state]).classList.add('active');
const stateDescriptions={
INITIALIZATION:!d.wifi_connected?'Waiting for Wi-Fi configuration or connection.':
(!d.rtu_ready?'Initializing the RTU client.':
(!d.tcp_bridge_ready?'Initializing the TCP-to-RTU bridge.':'Checking gateway readiness.')),
CONFIGURATION:'TCP-to-RTU forwarding is paused while configuration is active.',
DATA_TRANSFER:'Wi-Fi, RTU, and TCP are ready. Modbus requests can be forwarded.'
};
document.getElementById('state-description').textContent=stateDescriptions[d.state]||'Gateway state unavailable.';
document.getElementById('upt').textContent=d.uptime_s+'s';
document.getElementById('reqs').textContent=d.request_count;
document.getElementById('responses').textContent=d.response_count;
document.getElementById('errs').textContent=d.error_count;
}catch(e){}
}
async function pollLogs(){
try{let r=await fetch('/api/logs');if(!requireLogin(r)||!r.ok)return;let d=await r.json();document.getElementById('logbox').textContent=d.logs.join('\n');}catch(e){}
}
async function loadSettings(){
try{let r=await fetch('/api/settings');if(!requireLogin(r)||!r.ok)return;let d=await r.json();
baudSelect.value=String(d.baud);document.getElementById('data-bits').value=String(d.data_bits);
paritySelect.value=d.parity;document.getElementById('stop-bits').value=String(d.stop_bits);
document.getElementById('tcp-port').value=d.tcp_port;
document.getElementById('rtu-timeout-ms').value=d.rtu_timeout_ms ?? 500;
document.getElementById('tcp-timeout-ms').value=d.tcp_timeout_ms ?? 1000;
document.getElementById('ssid').value=d.ssid||'';
document.getElementById('network-mode').value=d.wifi_dhcp===false?'static':'dhcp';
document.getElementById('network-ip').value=d.wifi_static_ip||'';
document.getElementById('network-gateway').value=d.wifi_gateway||'';
document.getElementById('network-subnet').value=d.wifi_subnet||'';
document.getElementById('network-dns').value=d.wifi_dns||'';
document.getElementById('serial-log-level').value=String(d.serial_log_level??2);
document.getElementById('web-log-level').value=String(d.web_log_level??2);
updateNetworkFields();
}catch(e){}
}
function updateNetworkFields(){
const isStatic=document.getElementById('network-mode').value==='static';
document.getElementById('network-static-fields').hidden=!isStatic;
['network-ip','network-gateway','network-subnet','network-dns'].forEach(id=>{
document.getElementById(id).required=isStatic;
});
}
document.getElementById('network-mode').addEventListener('change',updateNetworkFields);
updateNetworkFields();
async function postJson(url,data){
const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
if(!requireLogin(response))throw new Error('Session expired. Please sign in again.');
const result=await response.json();if(!response.ok||!result.success)throw new Error(result.error||'Request failed.');return result;
}
function gatewaySettingsPayload(){
return{baud:+baudSelect.value,data_bits:+document.getElementById('data-bits').value,
parity:paritySelect.value,stop_bits:+document.getElementById('stop-bits').value,
tcp_port:+document.getElementById('tcp-port').value,
rtu_timeout_ms:+document.getElementById('rtu-timeout-ms').value,
tcp_timeout_ms:+document.getElementById('tcp-timeout-ms').value};
}
document.getElementById('serial-form').onsubmit=async e=>{
e.preventDefault();showToast('Saving serial settings...');
try{await postJson('/api/settings',gatewaySettingsPayload());
showToast('Settings saved. Rebooting to apply changes.','success');}catch(err){showToast(err.message,'error');}
};
document.getElementById('tcp-form').onsubmit=async e=>{
e.preventDefault();showToast('Saving Modbus TCP settings...');
try{await postJson('/api/settings',gatewaySettingsPayload());
showToast('Settings saved. Rebooting to apply changes.','success');}catch(err){showToast(err.message,'error');}
};
document.getElementById('auth-form').onsubmit=async e=>{
e.preventDefault();showToast('Updating login credentials...');
try{
await postJson('/api/auth/change',{
password:document.getElementById('auth-password').value
});
showToast('Credentials updated. Sign in again with the new credentials.','success');
setTimeout(()=>location.replace('/login'),700);
}catch(error){showToast(error.message||'Unable to update login credentials.','error');}
};
document.getElementById('logging-form').onsubmit=async e=>{
e.preventDefault();showToast('Saving logging settings...');
try{
await postJson('/api/logging?serial='+encodeURIComponent(document.getElementById('serial-log-level').value)+
'&web='+encodeURIComponent(document.getElementById('web-log-level').value),{});
showToast('Logging settings saved.','success');
}catch(error){showToast(error.message||'Unable to save logging settings.','error');}
};
document.getElementById('network-form').onsubmit=async e=>{
e.preventDefault();showToast('Saving Wi-Fi settings...');
const isDhcp=document.getElementById('network-mode').value==='dhcp';
try{await postJson('/api/wifi/config',{
ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,
dhcp:isDhcp,ip:document.getElementById('network-ip').value,
gateway:document.getElementById('network-gateway').value,
subnet:document.getElementById('network-subnet').value,
dns:document.getElementById('network-dns').value
});
showToast('Wi-Fi settings saved. The gateway is rebooting; reconnect to your Wi-Fi network and open its new IP address.','success');}catch(err){showToast(err.message,'error');}
};
async function loadOtaStatus(){
try{
const response=await fetch('/api/ota/status');if(!requireLogin(response)||!response.ok)return;
const status=await response.json(),capacity=document.getElementById('ota-capacity');
if(status.available)capacity.textContent=`Available update slot: ${Math.floor(status.maximum_image_bytes/1024)} KB maximum image size.`;
else capacity.textContent='OTA is unavailable because no inactive firmware slot is present. Reflash using USB with the OTA partition layout.';
document.getElementById('ota-upload').disabled=!status.available;
}catch(error){document.getElementById('ota-capacity').textContent='Unable to check OTA partition status.';}
}
document.getElementById('ota-form').onsubmit=e=>{
e.preventDefault();
const file=document.getElementById('ota-file').files[0],button=document.getElementById('ota-upload');
if(!file){showToast('Choose a firmware .bin file first.','error');return;}
const formData=new FormData();formData.append('firmware',file,file.name);
const progress=document.getElementById('ota-progress'),message=document.getElementById('ota-message');
button.disabled=true;progress.style.display='block';progress.value=0;message.textContent='Uploading firmware...';
const request=new XMLHttpRequest();request.open('POST','/api/ota');
request.upload.onprogress=event=>{
if(event.lengthComputable){progress.value=Math.round(event.loaded/event.total*100);message.textContent=`Uploading firmware: ${progress.value}%`;}
};
request.onload=()=>{
let result={};try{result=JSON.parse(request.responseText);}catch(error){}
if(request.status===401){message.textContent=result.error||'Login required.';showToast(message.textContent,'error');button.disabled=false;return;}
if(request.status>=200&&request.status<300&&result.success){
progress.value=100;message.textContent='Update verified. The gateway is restarting; wait for it to reconnect.';
showToast('Firmware update complete. The gateway is restarting.','success');
}else{
message.textContent=result.error||`Firmware upload failed (HTTP ${request.status}).`;
showToast(message.textContent,'error');button.disabled=false;
}
};
request.onerror=()=>{message.textContent='Upload connection failed. Check the gateway logs and try again.';showToast(message.textContent,'error');button.disabled=false;};
request.send(formData);
};
document.getElementById('rtu-diagnostic-form').onsubmit=async e=>{
e.preventDefault();
const button=document.getElementById('diag-send'),resultElement=document.getElementById('diag-result');
const unitId=Number(document.getElementById('diag-unit').value);
const functionCode=Number(document.getElementById('diag-register-type').value);
const address=Number(document.getElementById('diag-address').value);
const quantity=Number(document.getElementById('diag-count').value);
if(address+quantity>65536){
showToast('Starting address plus register count must not exceed 65536.','error');
return;
}
button.disabled=true;
resultElement.textContent=[
'Status: Waiting for RTU response...',
`Unit ID: ${unitId}`,
`Register type: ${functionCode===3?'Holding':'Input'}`,
`Starting address: ${address}`,
`Register count: ${quantity}`,
'RTU request is being built and transmitted by the gateway...',
'RTU RX: waiting...'
].join('\n');
try{
let response=await fetch('/api/diagnostics/rtu',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({unit_id:unitId,function_code:functionCode,address,quantity})});
let result=await readDiagnosticResponse(response);
if(response.status===202){
if(!result.diagnostic_id)throw new Error('Gateway accepted the diagnostic but did not return an operation ID.');
const diagnosticId=result.diagnostic_id,deadline=Date.now()+30000;
let retryDelay=250;
while(Date.now()<deadline){
try{
response=await fetch('/api/diagnostics/rtu/status?id='+encodeURIComponent(diagnosticId),{cache:'no-store'});
result=await readDiagnosticResponse(response);
if(response.ok&&result.pending){await new Promise(resolve=>setTimeout(resolve,retryDelay));retryDelay=Math.min(retryDelay+150,1000);continue;}
break;
}catch(error){
if(error instanceof TypeError&&Date.now()<deadline){
await new Promise(resolve=>setTimeout(resolve,retryDelay));retryDelay=Math.min(retryDelay+150,1000);continue;
}
throw error;
}
}
if(Date.now()>=deadline)throw new Error('Gateway did not return the RTU diagnostic result before the wait expired.');
}
resultElement.textContent=[
`Status: ${result.success?'Valid response':'No valid response'}`,
`Unit ID: ${result.unit_id||unitId}`,
`Register type: ${functionCode===3?'Holding':'Input'}`,
`Starting address: ${address}`,
`Register count: ${quantity}`,
`Request PDU: ${result.request_pdu||'not transmitted'}`,
`RTU TX (unit ID + PDU + CRC): ${result.request_rtu_frame||'not transmitted'}`,
`RTU RX (raw frame): ${result.response_rtu_frame||'no bytes received'}`,
result.registers?`Register values: ${result.registers.join(', ')}`:'',
result.response_pdu?`Response PDU: ${result.response_pdu}`:'',
result.error?`Error: ${result.error}`:''
].filter(Boolean).join('\n');
if(response.status===401){
resultElement.textContent+=`\n\nError: ${result.error||'Session expired. Sign in again to continue.'}`;
showToast('Session expired. Sign in again to continue.','error');
}else if(!result.success&&result.error!=='RTU timeout: no response received within 1 second')showToast(result.error||'RTU diagnostic did not receive a valid response.','error');
}catch(error){
resultElement.textContent=[
'Status: No diagnostic response received',
`Unit ID: ${unitId}`,
`Register type: ${functionCode===3?'Holding':'Input'}`,
`Starting address: ${address}`,
`Register count: ${quantity}`,
'Request/RTU TX details: gateway did not return a diagnostic result.',
'RTU RX: no response received',
`HTTP connection error: ${error.message||'Unable to reach the gateway.'}`,
'RTU result: unknown because the gateway did not return an HTTP response.'
].join('\n');
showToast('Could not contact the gateway for the diagnostic result. This is an HTTP connection failure, not an RTU timeout.','error');
}
finally{button.disabled=false;}
};
async function readDiagnosticResponse(response){
const body=await response.text();
try{return JSON.parse(body);}
catch(error){throw new Error(`Gateway returned HTTP ${response.status} without a readable diagnostic result: ${body.slice(0,160)||'empty response'}`);}
}
async function clearLogs(){
try{await fetch('/api/logs/clear',{method:'POST'});await pollLogs();}catch(e){}
}
async function rebootDevice(){
try{
await postJson('/api/reboot',{});
showToast('Reboot requested.','success');
}catch(error){showToast(error.message||'Unable to reboot gateway.','error');}
}
async function logout(){
try{await fetch('/api/logout',{method:'POST',cache:'no-store'});}
catch(error){}
location.replace('/login?logged_out=1');
}
async function factoryReset(){
if(!confirm('Factory reset will erase saved Wi-Fi and gateway settings, then reboot. Continue?')) return;
try{
showToast('Restoring factory defaults...');
await postJson('/api/factory-reset',{});showToast('Factory reset complete. Rebooting; reconnect to AP-modbus-gw.','success');
}catch(e){showToast(e.message||'Factory reset failed.','error');}
}
setInterval(pollStatus,2000);setInterval(pollLogs,3000);
pollStatus();pollLogs();loadSettings();loadOtaStatus();
</script></body></html>
)rawliteral";
