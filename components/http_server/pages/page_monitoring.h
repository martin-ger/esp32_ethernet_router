/* Monitoring page templates - PCAP, NetFlow, Syslog */

#define MONITORING_CHUNK_HEAD "<html>\
<head>\
<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0'>\
<meta charset='UTF-8'>\
<title>Monitoring</title>\
<link rel='icon' href='favicon.png'>\
</head>\
<style>\
* { box-sizing: border-box; margin: 0; padding: 0; }\
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif; background: linear-gradient(135deg, #0d1a0d 0%, #0a1f0a 100%); color: #e0e0e0; padding: 1rem; min-height: 100vh; line-height: 1.6; }\
h1 { font-size: 1.5rem; font-weight: 600; color: #00e676; margin-bottom: 1rem; text-shadow: 0 0 20px rgba(0, 230, 118, 0.3); }\
h2 { font-size: 1.15rem; font-weight: 500; color: #00e676; margin: 1.5rem 0 0.75rem 0; padding-bottom: 0.5rem; border-bottom: 1px solid rgba(0, 230, 118, 0.2); }\
#container { max-width: 500px; margin: 0 auto; padding: 1.5rem; background: rgba(20, 36, 20, 0.9); border-radius: 16px; box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4); backdrop-filter: blur(10px); }\
form { margin-bottom: 1.5rem; }\
table { width: 100%; border-collapse: collapse; }\
td { padding: 0.5rem 0; vertical-align: top; }\
td:first-child { color: #888; font-size: 0.9rem; padding-right: 0.75rem; width: 35%; text-align: right; }\
input[type='text'], input[type='password'], input[type='number'] { width: 100%; background: rgba(16, 32, 16, 0.6); border: 1px solid rgba(0, 230, 118, 0.2); border-radius: 8px; color: #e0e0e0; padding: 0.75rem; font-size: 0.95rem; }\
input[type='text']:focus, input[type='password']:focus, input[type='number']:focus, select:focus { outline: none; border-color: #00e676; box-shadow: 0 0 0 3px rgba(0, 230, 118, 0.1); background: rgba(16, 32, 16, 0.8); }\
input::placeholder { color: #666; }\
select { width: 100%; background: rgba(16, 32, 16, 0.6); border: 1px solid rgba(0, 230, 118, 0.2); border-radius: 8px; color: #e0e0e0; padding: 0.75rem; font-size: 0.95rem; cursor: pointer; -webkit-appearance: none; -moz-appearance: none; appearance: none; background-image: url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%2300e676' stroke-width='1.5' fill='none'/%3E%3C/svg%3E\"); background-repeat: no-repeat; background-position: right 0.75rem center; padding-right: 2rem; }\
select option { background: #0a1f0a; color: #e0e0e0; }\
input[type='checkbox'], input[type='radio'] { -webkit-appearance: none; -moz-appearance: none; appearance: none; width: 18px; height: 18px; border: 2px solid rgba(0, 230, 118, 0.3); border-radius: 4px; background: rgba(16, 32, 16, 0.6); cursor: pointer; vertical-align: middle; position: relative; flex-shrink: 0; }\
input[type='radio'] { border-radius: 50%; }\
input[type='checkbox']:checked, input[type='radio']:checked { background: #00e676; border-color: #00e676; }\
input[type='checkbox']:checked::after { content: ''; position: absolute; left: 4px; top: 1px; width: 6px; height: 10px; border: solid #0d1a0d; border-width: 0 2px 2px 0; transform: rotate(45deg); }\
input[type='radio']:checked::after { content: ''; position: absolute; left: 3px; top: 3px; width: 8px; height: 8px; border-radius: 50%; background: #0d1a0d; }\
input[type='checkbox']:focus, input[type='radio']:focus { outline: none; border-color: #00e676; box-shadow: 0 0 0 3px rgba(0, 230, 118, 0.1); }\
.ok-button, .red-button { border: none; border-radius: 8px; padding: 0.75rem 1.5rem; font-size: 0.95rem; font-weight: 600; cursor: pointer; width: 100%; margin-top: 0.5rem; }\
.ok-button { background: linear-gradient(135deg, #43a047 0%, #2e7d32 100%); color: #fff; box-shadow: 0 4px 15px rgba(67, 160, 71, 0.4); }\
.red-button { background: linear-gradient(135deg, #e57373 0%, #f5576c 100%); color: #fff; box-shadow: 0 4px 15px rgba(245, 87, 108, 0.4); }\
small { display: block; color: #888; font-size: 0.85rem; margin-top: 0.5rem; line-height: 1.4; }\
@media (max-width: 600px) { body { padding: 0.5rem; } #container { padding: 1rem; } h1 { font-size: 1.25rem; } h2 { font-size: 1rem; } td:first-child { font-size: 0.8rem; width: 40%; } input[type='text'], input[type='password'], input[type='number'], select { font-size: 0.9rem; padding: 0.65rem; } select { padding-right: 1.75rem; } .ok-button, .red-button { font-size: 0.9rem; padding: 0.65rem 1.25rem; } }\
</style>\
<body>\
<div id='container'>\
<div style='display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem;'>\
<div style='display: flex; align-items: center;'>\
<a href='/' style='display: inline-block; margin-right: 1rem;'><img src='/favicon.png' alt='Home' style='width: 64px; height: 64px; border: none;'></a>\
<h1 style='margin: 0;'>Monitoring</h1>\
</div>"

/* PCAP capture section - uses: off_sel, acl_sel, promisc_sel, client_color, client_text, captured, dropped, snaplen, sta_ip */
#define MONITORING_CHUNK_PCAP "\
<h2>PCAP Packet Capture</h2>\
<form action='' method='GET'>\
<input type='hidden' name='pcap_save' value='1'/>\
<table>\
<tr><td>Mode</td><td>\
<select name='pcap_mode'>\
<option value='off' %s>Off</option>\
<option value='acl' %s>ACL Monitor</option>\
<option value='promisc' %s>Promiscuous</option>\
</select>\
</td></tr>\
<tr><td>Client</td><td><strong style='color: %s;'>%s</strong></td></tr>\
<tr><td>Stats</td><td>%lu captured, %lu dropped</td></tr>\
<tr><td>Snaplen</td><td><input type='text' name='pcap_snaplen' value='%d' placeholder='64-1600'/></td></tr>\
<tr><td></td><td><input type='submit' value='Save' class='ok-button'/></td></tr>\
</table>\
<small>Connect using: nc %s 19000 | wireshark -k -i -</small>\
</form>"

/* NetFlow v5 exporter section - uses: ingress_chk, egress_chk, collector_ip, port, idle_to, active_to, active_flows, exported_flows */
#define MONITORING_CHUNK_NETFLOW "\
<h2>NetFlow v5 Exporter</h2>\
<form action='' method='GET'>\
<input type='hidden' name='nf_save' value='1'/>\
<table>\
<tr><td>Capture</td><td>\
<label style='display:block; margin-bottom:0.4rem;'><input type='checkbox' name='nf_ingress' value='1' %s> Ingress (LAN&rarr;router)</label>\
<label style='display:block;'><input type='checkbox' name='nf_egress' value='1' %s> Egress (router&rarr;LAN)</label>\
</td></tr>\
<tr><td>Collector IP</td><td><input type='text' name='nf_collector' value='%s' placeholder='e.g. 192.168.1.10'/></td></tr>\
<tr><td>UDP Port</td><td><input type='number' name='nf_port' value='%u' min='1' max='65535' style='width: 100px;'/></td></tr>\
<tr><td>Idle Timeout</td><td><input type='number' name='nf_idle_to' value='%lu' min='1' max='3600' style='width: 100px;'/> sec</td></tr>\
<tr><td>Active Timeout</td><td><input type='number' name='nf_active_to' value='%lu' min='1' max='86400' style='width: 100px;'/> sec</td></tr>\
<tr><td>Stats</td><td>%lu active flows, %lu exported</td></tr>\
<tr><td></td><td><input type='submit' value='Save' class='ok-button'/></td></tr>\
</table>\
<small>Exports Ethernet downlink flows to a NetFlow v5 UDP collector. Uncheck both to disable.</small>\
</form>"

/* Syslog section - uses: status_color, status_text, server, port */
#define MONITORING_CHUNK_SYSLOG "\
<h2>Syslog</h2>\
<form action='' method='GET'>\
<input type='hidden' name='syslog_save' value='1'/>\
<table>\
<tr><td>Status</td><td><strong style='color: %s;'>%s</strong></td></tr>\
<tr><td>Server</td><td><input type='text' name='syslog_server' value='%s' placeholder='IP or hostname'/></td></tr>\
<tr><td>UDP Port</td><td><input type='number' name='syslog_port' value='%u' min='1' max='65535' style='width: 100px;'/></td></tr>\
<tr><td></td><td><input type='submit' value='Save' class='ok-button'/></td></tr>\
</table>\
<small>Forwards ESP-IDF log output to a remote syslog server (UDP). Leave server empty to disable.</small>\
</form>"

/* Closing HTML - closes #container */
#define MONITORING_CHUNK_TAIL "\
<div style='margin-top: 2rem; text-align: center;'>\
<a href='/' style='padding: 0.75rem 2rem; background: linear-gradient(135deg, #43a047 0%, #2e7d32 100%); color: #fff; border: none; border-radius: 8px; text-decoration: none; font-size: 0.95rem; font-weight: 600;'>&#127968; Home</a>\
</div>\
</div>\
</body></html>"
