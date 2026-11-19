#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>
#include "app_config.h"
#include "web_server.h"
#include "../common/resolution.h"

namespace vcam {
namespace {
constexpr uintptr_t invalid_socket = static_cast<uintptr_t>(-1);
constexpr size_t max_header = 8192;
constexpr size_t max_body = 512 * 1024;

const char page[] = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Virtual Camera</title><style>
:root{color-scheme:dark;--bg:#101416;--panel:#171d20;--line:#334047;--muted:#9aa9ae;--text:#e7eeee;--accent:#93d3c3;--bad:#ef9b8f}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(135deg,#0d1113,#151c1f);color:var(--text);font:14px/1.5 "Segoe UI",sans-serif}
main{max-width:1100px;margin:36px auto;padding:0 20px}.top{display:flex;justify-content:space-between;align-items:end;border-bottom:1px solid var(--line);padding-bottom:18px;margin-bottom:22px}
h1{font-size:22px;font-weight:500;letter-spacing:.04em;margin:0}.sub,.muted{color:var(--muted)}.layout{display:grid;grid-template-columns:minmax(300px,390px) 1fr;gap:18px}
section{background:var(--panel);border:1px solid var(--line);padding:20px;position:relative}section:before{content:"";position:absolute;left:-1px;top:-1px;width:26px;border-top:2px solid var(--accent)}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:var(--accent);font-weight:500;margin:0 0 18px}
label{display:block;color:var(--muted);font-size:12px;margin:14px 0 6px}input,select{width:100%;background:#101517;color:var(--text);border:1px solid #3a484e;padding:10px 11px;font:inherit;outline:none}input:focus,select:focus{border-color:var(--accent)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.actions{display:flex;gap:9px;margin-top:18px}button{background:transparent;color:var(--accent);border:1px solid var(--accent);padding:9px 14px;font:inherit;cursor:pointer}button:hover{background:#243733}button.secondary{color:var(--text);border-color:var(--line)}
.state{display:flex;gap:8px;align-items:center}.dot{width:8px;height:8px;border:1px solid var(--accent);border-radius:50%;display:inline-block}.dot.off{border-color:var(--bad);background:var(--bad)}.statgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:18px}.stat{border-top:1px solid var(--line);padding:10px 0}.stat b{display:block;font-weight:500;margin-top:3px}.preview-meta{display:flex;justify-content:space-between;border:1px solid var(--line);border-bottom:0;padding:10px 12px;color:var(--muted)}.preview-meta b{color:var(--accent);font-weight:500;margin-left:6px}.preview{min-height:280px;border:1px solid var(--line);background:#0a0d0e;display:grid;place-items:center;overflow:hidden}.preview canvas{width:100%;height:auto;max-height:65vh;object-fit:contain}.message{min-height:22px;margin-top:12px;color:var(--accent)}.message.error{color:var(--bad)}
textarea{width:100%;min-height:120px;resize:vertical;background:#101517;color:var(--text);border:1px solid #3a484e;padding:10px;font:12px/1.45 Consolas,monospace}.formats label{display:flex;align-items:center;gap:8px}.formats input{width:auto}details{border-top:1px solid var(--line);margin-top:18px;padding-top:12px}summary{color:var(--accent);cursor:pointer}
.toggle{display:flex;align-items:center;gap:8px}.toggle input{width:auto}.logs{max-width:1100px;margin:18px auto;padding:16px 20px;background:var(--panel);border:1px solid var(--line)}.log-tools{display:flex;justify-content:space-between;align-items:center}.logs pre{max-height:38vh;min-height:90px;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere;background:#0b0e10;border:1px solid var(--line);padding:12px;color:#c7d4d6;font:12px/1.45 Consolas,monospace}
.network-access{margin-top:16px;border:1px solid var(--line);padding:12px}.network-row{grid-template-columns:1fr auto;align-items:center}.network-row button.danger{color:var(--bad);border-color:var(--bad)}.network-warning{margin:10px 0 0;padding:9px 11px;border-left:2px solid var(--bad);background:#2a1717;color:var(--bad)}.network-warning[hidden]{display:none}
@media(max-width:760px){main{margin:20px auto}.layout{grid-template-columns:1fr}.top{align-items:start;gap:8px;flex-direction:column}}
</style><!--VCAM_THEME--></head><body><main>
<header class="top"><div><h1>VIRTUAL CAMERA / CONTROL</h1><div class="sub">本机设备状态与运行参数</div></div><div id="webstate" class="state"><i class="dot"></i><span>连接中</span></div></header>
<div class="layout"><section><h2>运行设置</h2>
<label for="camera">摄像头</label><select id="camera"><option value="1">摄像头 1</option><option value="2">摄像头 2</option></select>
<div class="row"><div><label for="width">宽度</label><input id="width" type="number" min="2" max="4096" step="2"></div><div><label for="height">高度</label><input id="height" type="number" min="1" max="2160"></div></div>
<label for="fps">默认帧率上限（1–60 fps）</label><input id="fps" type="number" min="1" max="60" step="1">
<div class="row"><div><label for="vid">VID（十六进制）</label><input id="vid" maxlength="4" placeholder="bb10"></div><div><label for="pid">PID（十六进制）</label><input id="pid" maxlength="4" placeholder="cc10"></div></div>
<div class="row"><div><label for="manufacturer">设备厂商</label><input id="manufacturer" maxlength="126"></div><div><label for="name">摄像头名称</label><input id="name" maxlength="126"></div></div>
<label>输出格式</label><div class="row formats"><label><input id="yuy2" type="checkbox"> YUY2</label><label><input id="mjpeg" type="checkbox"> MJPEG</label></div>
<label for="rgb">RGB 输入管道名</label><input id="rgb" autocomplete="off">
<label for="control">控制管道名</label><input id="control" autocomplete="off">
<label for="access">控制管道访问范围</label><select id="access"><option value="everyone">Everyone + SYSTEM</option><option value="owner">当前用户 + SYSTEM</option><option value="users">本机 Users 组 + SYSTEM</option></select>
<div class="network-access"><div class="row network-row"><div><b>WebUI 网络监听</b><div class="sub">默认仅本机访问；可临时允许其他设备连接。</div></div><button id="networkToggle" class="secondary" type="button">允许网络访问</button></div><div id="networkWarning" class="network-warning" hidden>危险：WebUI 正在监听所有 IPv4 网络接口。访问仍需本次启动令牌，但 HTTP 不加密；请只在可信网络开启。其他设备使用本机 IPv4 地址、端口和控制台中的令牌访问。</div></div>
<label class="toggle"><input id="hideWindow" type="checkbox">隐藏控制台窗口到系统托盘</label><div class="sub">隐藏后程序继续运行并收集日志；双击托盘图标或右键菜单可切回。此开关只影响当前运行进程。</div>
<details><summary>自定义页面主题</summary><label for="customCss">CSS</label><textarea id="customCss" spellcheck="false" maxlength="65536" placeholder="/* CSS 注入到页面 head */"></textarea><label for="customJs">JavaScript</label><textarea id="customJs" spellcheck="false" maxlength="65536" placeholder="// JavaScript 注入到页面 head"></textarea><div class="sub">JavaScript 在 head 中同步执行，DOM 操作可放进 DOMContentLoaded 回调。自定义脚本可改写本机管理页行为，只粘贴自己信任的代码。</div></details>
<div class="actions"><button id="apply">应用并保存</button><button id="refresh" class="secondary">刷新状态</button></div><div id="message" class="message"></div>
</section><section><h2>设备与管道状态</h2><div class="statgrid" id="stats"></div><div class="preview-meta"><span>USB 模式 <b id="previewMode">—</b></span><span>实际帧率 <b id="actualFps">0.0 fps</b></span></div><div class="preview"><canvas id="frame" width="640" height="480"></canvas></div><div class="sub" style="margin-top:10px">预览使用当前摄像头的 YUY2 画面。</div></section></div>
</main><details class="logs" id="logDetails"><summary>运行日志</summary><div class="log-tools"><span class="sub">最近日志</span><button id="refreshLogs" class="secondary">刷新日志</button></div><pre id="logOutput" aria-live="polite"></pre></details><script>
const fragment=location.hash.slice(1).split(':');const token=fragment[0]||'';const initialCamera=fragment[1]==='2'?'2':'1';history.replaceState(null,"",location.pathname);let current=null;
const $=id=>document.getElementById(id);const api=async(path,opts={})=>fetch(path,{...opts,headers:{Authorization:`Bearer ${token}`,...(opts.headers||{})}});
$("camera").value=initialCamera;
function state(ok,text){$("webstate").innerHTML=`<i class="dot ${ok?"":"off"}"></i><span>${text}</span>`}
function updateNetworkUI(enabled){$("networkToggle").textContent=enabled?'关闭网络访问':'允许网络访问';$("networkToggle").classList.toggle('danger',enabled);$("networkWarning").hidden=!enabled}
function escapeHtml(value){return String(value??'').replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]))}
async function refresh(loadConfig=false){try{const r=await api('/api/status');if(!r.ok)throw Error('接口未授权或不可用');current=await r.json();state(true,'本机服务在线');updateNetworkUI(Boolean(current.webNetworkAccess));const cam=current.cameras.find(c=>c.index===Number($("camera").value));if(loadConfig&&cam){$("width").value=cam.width;$("height").value=cam.height;$("fps").value=cam.fps;$("vid").value=cam.vid;$("pid").value=cam.pid;$("manufacturer").value=cam.manufacturer;$("name").value=cam.name;$("yuy2").checked=cam.yuy2;$("mjpeg").checked=cam.mjpeg}if(loadConfig){$("camera").options[1].disabled=!current.cameras[1].present;$("rgb").value=current.rgbPipe.name;$("control").value=current.controlPipe.name;$("access").value=current.controlPipeAccess;const themeResponse=await api('/api/theme');if(themeResponse.ok){const theme=await themeResponse.json();$("customCss").value=theme.css;$("customJs").value=theme.js}const windowResponse=await api('/api/window');if(windowResponse.ok){const windowState=await windowResponse.json();$("hideWindow").checked=windowState.hidden}}
const pipeState=!current.rgbPipe.active?'当前输入源未使用':current.rgbPipe.connected?'客户端已连接':current.rgbPipe.listening?'监听中，等待客户端连接':'监听未运行';const frameState=current.rgbPipe.frames?`${current.rgbPipe.frames} 帧 · 最近 ${Math.round(current.rgbPipe.frameAgeMs/1000)} 秒`:'尚未收到帧';
const driverLabel={foss:'FOSS 重建版',original:'原版驱动',unknown:'已发现·无法识别',unavailable:'未发现'}[current.driverKind]||'无法识别';const modeLabel={superspeed:'SuperSpeed','high-speed':'USB 2.0 高速',disconnected:'未连接'}[cam?.usbMode]||'未知';const cameraState=cam?.running?'运行中':cam?.present?'已停止':'未连接';$("previewMode").textContent=modeLabel;$("actualFps").textContent=`${Number(cam?.actualFps||0).toFixed(1)} fps`;
$("stats").innerHTML=`<div class="stat"><span class="muted">虚拟 USB 驱动</span><b>${escapeHtml(driverLabel)}</b></div><div class="stat"><span class="muted">摄像头状态</span><b>${escapeHtml(cameraState)}</b></div><div class="stat"><span class="muted">USB 身份</span><b>${escapeHtml(cam?.manufacturer||'')} ${escapeHtml(cam?.vid||'')}:${escapeHtml(cam?.pid||'')}</b></div><div class="stat"><span class="muted">RGB 输入管道</span><b>${escapeHtml(pipeState)}</b><small class="muted">${escapeHtml(frameState)}</small></div><div class="stat"><span class="muted">控制管道</span><b>${current.controlPipe.running?'监听中':'未运行'}</b></div><div class="stat"><span class="muted">Web UI</span><b>${current.webNetworkAccess?'网络监听·所有 IPv4 接口':'仅本机·127.0.0.1'}:${escapeHtml(current.webPort)}</b></div>`
}catch(e){state(false,'状态读取失败');$("message").textContent=e.message}}
function clip(v){return Math.max(0,Math.min(255,v))}
function draw(bytes,w,h){const canvas=$("frame"),ctx=canvas.getContext('2d');canvas.width=w;canvas.height=h;const image=ctx.createImageData(w,h),out=image.data;for(let i=0,j=0;i<w*h*2;i+=4,j+=8){const u=bytes[i+1]-128,v=bytes[i+3]-128;for(let p=0;p<2;p++){const y=Math.max(0,bytes[i+p*2]-16),c=298*y;out[j+p*4]=clip((c+409*v+128)>>8);out[j+p*4+1]=clip((c-100*u-208*v+128)>>8);out[j+p*4+2]=clip((c+516*u+128)>>8);out[j+p*4+3]=255}}ctx.putImageData(image,0,0)}
async function preview(){try{const r=await api(`/api/frame?camera=${$("camera").value}`);if(!r.ok)return;const w=Number(r.headers.get('X-Frame-Width')),h=Number(r.headers.get('X-Frame-Height'));draw(new Uint8Array(await r.arrayBuffer()),w,h)}catch(_){}}
async function refreshLogs(){try{const r=await api('/api/logs');if(!r.ok)return;const value=await r.json();const output=$("logOutput"),stick=output.scrollTop+output.clientHeight>=output.scrollHeight-8;output.textContent=value.text||'';if(stick)output.scrollTop=output.scrollHeight}catch(_){}}
$("camera").onchange=()=>refresh(true);$("refresh").onclick=()=>refresh(true);$("apply").onclick=async()=>{const message=$("message");message.className='message';message.textContent='应用中…';const body=new URLSearchParams({camera:$("camera").value,width:$("width").value,height:$("height").value,fps:$("fps").value,vid:$("vid").value,pid:$("pid").value,manufacturer:$("manufacturer").value,name:$("name").value,yuy2:String($("yuy2").checked),mjpeg:String($("mjpeg").checked),rgb_pipe:$("rgb").value,control_pipe:$("control").value,control_pipe_access:$("access").value,custom_css:$("customCss").value,custom_js:$("customJs").value});try{const r=await api('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const result=await r.json();if(!r.ok||!result.ok)throw Error(result.error||'应用失败');message.textContent='设置已保存。摄像头由用户态摘除并重新枚举，无需重新安装驱动；视频客户端可能需要重新打开。主题正在刷新。';location.replace('/#'+token+':'+$("camera").value)}catch(e){message.className='message error';message.textContent=e.message}};
$("networkToggle").onclick=async()=>{const button=$("networkToggle"),message=$("message"),enabled=!Boolean(current?.webNetworkAccess);button.disabled=true;message.className='message';message.textContent=enabled?'正在开启网络监听…':'正在关闭网络监听…';try{const r=await api('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({enabled:String(enabled)})});const result=await r.json();if(!r.ok||!result.ok)throw Error(result.error||'网络访问设置失败');current.webNetworkAccess=result.enabled;updateNetworkUI(result.enabled);message.textContent=result.enabled?'网络访问已开启。其他设备请使用本机 IPv4 地址、当前端口和启动令牌。':'网络访问已关闭，仅允许本机连接。'}catch(e){message.className='message error';message.textContent=e.message}finally{button.disabled=false}};
$("hideWindow").onchange=async()=>{const requested=$("hideWindow").checked;try{const r=await api('/api/window',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({hidden:String(requested)})});const value=await r.json();if(!r.ok||!value.ok)throw Error(value.error||'托盘切换失败');$("hideWindow").checked=value.hidden}catch(e){$("hideWindow").checked=!requested;$("message").className='message error';$("message").textContent=e.message}};
$("logDetails").addEventListener('toggle',()=>{if($("logDetails").open)refreshLogs()});$("refreshLogs").onclick=refreshLogs;
if(!token)state(false,'请从程序控制台打开带令牌的地址');refresh(true);setInterval(()=>{refresh();preview()},1000);setInterval(()=>{if($("logDetails").open)refreshLogs()},2000);preview();
</script></body></html>)HTML";

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
SOCKET create_listener(unsigned short port, bool network_access) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return INVALID_SOCKET;
    BOOL exclusive = TRUE;
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
        closesocket(listener);
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(network_access ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        closesocket(listener);
        return INVALID_SOCKET;
    }
    return listener;
}
bool request_host_allowed(SOCKET client, const std::string& host, unsigned short port, bool network_access) {
    const std::string loopback = "127.0.0.1:" + std::to_string(port);
    sockaddr_in local{};
    int local_length = sizeof(local);
    if (getsockname(client, reinterpret_cast<sockaddr*>(&local), &local_length) == SOCKET_ERROR) return false;
    if (ntohl(local.sin_addr.s_addr) >> 24 == 127)
        return host == loopback || host == "localhost:" + std::to_string(port);
    if (!network_access) return false;
    char address[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &local.sin_addr, address, sizeof(address))) return false;
    return host == std::string(address) + ":" + std::to_string(port);
}
std::string protect_raw_text_close(std::string source, const char* closing_tag) {
    const size_t length = std::char_traits<char>::length(closing_tag);
    for (size_t position = 0; position + length <= source.size(); ++position) {
        bool matches = true;
        for (size_t index = 0; index < length; ++index) {
            if (std::tolower(static_cast<unsigned char>(source[position + index])) !=
                std::tolower(static_cast<unsigned char>(closing_tag[index]))) {
                matches = false;
                break;
            }
        }
        if (matches) {
            source.insert(position + 1, "\\");
            position += length;
        }
    }
    return source;
}
bool send_all(SOCKET socket, const char* data, size_t length) {
    while (length) {
        const int sent = send(socket, data, static_cast<int>((std::min)(length, size_t(1024 * 1024))), 0);
        if (sent <= 0) return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}
void respond(SOCKET socket, int status, const char* reason, const char* type,
             const std::string& body, const std::string& extra = {}) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Type: " << type
           << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n"
           << "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
           << "Referrer-Policy: no-referrer\r\n" << extra << "\r\n";
    const std::string value = header.str();
    send_all(socket, value.data(), value.size());
    send_all(socket, body.data(), body.size());
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool form_decode(const std::string& input, std::map<std::string, std::string>& result) {
    size_t begin = 0;
    while (begin < input.size()) {
        const size_t end = input.find('&', begin);
        const std::string item = input.substr(begin, end == std::string::npos ? end : end - begin);
        const size_t equal = item.find('=');
        if (equal == std::string::npos) return false;
        auto decode = [](const std::string& value, std::string& decoded) {
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '+') decoded.push_back(' ');
                else if (value[i] == '%') {
                    if (i + 2 >= value.size()) return false;
                    const int high = hex_digit(value[i+1]), low = hex_digit(value[i+2]);
                    if (high < 0 || low < 0) return false;
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                } else decoded.push_back(value[i]);
            }
            return decoded.find('\0') == std::string::npos;
        };
        std::string key, value;
        if (!decode(item.substr(0, equal), key) || !decode(item.substr(equal + 1), value)) return false;
        result[key] = value;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}
bool parse_integer(const std::map<std::string, std::string>& fields, const char* key, int& value) {
    const auto found = fields.find(key);
    if (found == fields.end() || found->second.empty()) return false;
    const char* begin = found->second.data();
    const char* end = begin + found->second.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}
bool parse_hex_id(const std::map<std::string, std::string>& fields, const char* key, uint16_t& value) {
    const auto found = fields.find(key);
    if (found == fields.end() || found->second.empty()) return false;
    std::string text = found->second;
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text.erase(0, 2);
    unsigned int parsedValue = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsedValue, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || parsedValue == 0 || parsedValue > 0xffff)
        return false;
    value = static_cast<uint16_t>(parsedValue);
    return true;
}
bool parse_bool(const std::map<std::string, std::string>& fields, const char* key, bool& value) {
    const auto found = fields.find(key);
    if (found == fields.end()) return false;
    if (found->second == "true") value = true;
    else if (found->second == "false") value = false;
    else return false;
    return true;
}
std::string json_escape(const std::string& value) {
    std::string out;
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            out += "\\u00";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}
struct Request { std::string method, target, version, body; std::map<std::string, std::string> headers; };
bool receive_request(SOCKET socket, Request& request) {
    std::string data;
    char buffer[2048];
    size_t header_end = std::string::npos, content_length = 0;
    for (;;) {
        if (data.size() > max_header + max_body) return false;
        const int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        data.append(buffer, count);
        header_end = data.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (data.size() > max_header) return false;
            continue;
        }
        if (header_end > max_header) return false;
        const size_t first_line = data.find("\r\n");
        if (first_line == std::string::npos || first_line > header_end) return false;
        std::istringstream line(data.substr(0, first_line));
        if (!(line >> request.method >> request.target >> request.version)) return false;
        size_t line_start = first_line + 2;
        while (line_start < header_end) {
            const size_t line_end = data.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) return false;
            const size_t colon = data.find(':', line_start);
            if (colon == std::string::npos || colon > line_end) return false;
            std::string key = lower(data.substr(line_start, colon - line_start));
            size_t value_start = colon + 1;
            while (value_start < line_end && data[value_start] == ' ') ++value_start;
            request.headers[key] = data.substr(value_start, line_end - value_start);
            line_start = line_end + 2;
        }
        if (request.headers.count("transfer-encoding")) return false;
        if (request.headers.count("content-length")) {
            const auto& value = request.headers.at("content-length");
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || content_length > max_body) return false;
        }
        const size_t body_start = header_end + 4;
        while (data.size() - body_start < content_length) {
            const int more = recv(socket, buffer, sizeof(buffer), 0);
            if (more <= 0) return false;
            data.append(buffer, more);
            if (data.size() > max_header + max_body) return false;
        }
        request.body = data.substr(body_start, content_length);
        return true;
    }
}
}

WebServer::~WebServer() { stop(); }
bool WebServer::start(unsigned short port, std::string token,
                      std::function<std::string()> status,
                      std::function<std::optional<CameraFrame>(int)> frame,
                      std::function<bool(const WebUpdate&, std::string&)> update,
                      std::function<std::pair<std::string, std::string>()> theme,
                      std::function<std::string()> logs,
                      std::function<bool()> window_hidden,
                      std::function<bool(bool)> set_window_hidden,
                      bool network_access,
                      std::function<bool(bool, std::string&)> set_network_access) {
    if (worker_.joinable() || token.size() < 32 || !status || !frame || !update) return false;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    winsock_initialized_.store(true);
    SOCKET listener = create_listener(port, network_access);
    if (listener == INVALID_SOCKET) {
        winsock_initialized_.store(false);
        WSACleanup();
        return false;
    }
    port_ = port;
    token_ = std::move(token);
    status_ = std::move(status);
    frame_ = std::move(frame);
    update_ = std::move(update);
    theme_ = std::move(theme);
    logs_ = std::move(logs);
    window_hidden_ = std::move(window_hidden);
    set_window_hidden_ = std::move(set_window_hidden);
    set_network_access_ = std::move(set_network_access);
    network_access_.store(network_access);
    stopping_.store(false);
    running_.store(true);
    listener_.store(static_cast<uintptr_t>(listener));
    try { worker_ = std::thread(&WebServer::run, this); }
    catch (...) {
        running_.store(false);
        listener_.store(invalid_socket);
        closesocket(listener);
        winsock_initialized_.store(false);
        WSACleanup();
        return false;
    }
    return true;
}
void WebServer::stop() noexcept {
    stopping_.store(true);
    {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        const uintptr_t value = listener_.exchange(invalid_socket);
        if (value != invalid_socket) {
            const SOCKET listener = static_cast<SOCKET>(value);
            shutdown(listener, SD_BOTH);
            closesocket(listener);
        }
    }
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    if (winsock_initialized_.exchange(false)) WSACleanup();
}

bool WebServer::rebind_network_access(bool enabled, std::string& error) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    if (stopping_.load()) { error = "web-server-stopping"; return false; }
    const bool previous = network_access_.load();
    if (enabled == previous) return true;
    if (!set_network_access_) { error = "network-setting-unavailable"; return false; }

    const uintptr_t old_value = listener_.exchange(invalid_socket);
    if (old_value != invalid_socket) {
        const SOCKET old_listener = static_cast<SOCKET>(old_value);
        shutdown(old_listener, SD_BOTH);
        closesocket(old_listener);
    }
    auto restore_listener = [&] {
        SOCKET restored = create_listener(port_, previous);
        if (restored == INVALID_SOCKET) {
            stopping_.store(true);
            error += "; previous-listener-restore-failed";
            return false;
        }
        listener_.store(static_cast<uintptr_t>(restored));
        return true;
    };

    SOCKET replacement = create_listener(port_, enabled);
    if (replacement == INVALID_SOCKET) {
        error = "web-listener-rebind-failed";
        restore_listener();
        return false;
    }
    if (stopping_.load()) {
        closesocket(replacement);
        error = "web-server-stopping";
        return false;
    }
    if (!set_network_access_(enabled, error)) {
        closesocket(replacement);
        if (error.empty()) error = "network-setting-save-failed";
        restore_listener();
        return false;
    }
    network_access_.store(enabled);
    listener_.store(static_cast<uintptr_t>(replacement));
    return true;
}

void WebServer::run() noexcept {
    try {
        while (!stopping_.load()) {
            const uintptr_t listener_value = listener_.load();
            if (listener_value == invalid_socket) break;
            const SOCKET listener = static_cast<SOCKET>(listener_value);
            sockaddr_in client_address{};
            int address_size = sizeof(client_address);
            SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&client_address), &address_size);
            if (client == INVALID_SOCKET) {
                if (stopping_.load()) break;
                if (listener_.load() != listener_value) continue;
                break;
            }
            DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            Request request;
            if (!receive_request(client, request)) {
                respond(client, 400, "Bad Request", "text/plain; charset=utf-8", "bad request");
                closesocket(client);
                continue;
            }
            const auto host = request.headers.find("host");
            const auto origin = request.headers.find("origin");
            const bool host_allowed = host != request.headers.end() &&
                request_host_allowed(client, host->second, port_, network_access_.load());
            const bool origin_allowed = origin == request.headers.end() ||
                (host != request.headers.end() && origin->second == "http://" + host->second);
            if (!host_allowed || !origin_allowed) {
                respond(client, 403, "Forbidden", "text/plain; charset=utf-8", "forbidden");
                closesocket(client);
                continue;
            }
            const auto authorization = request.headers.find("authorization");
            const bool root_page = request.method == "GET" && request.target == "/";
            if (!root_page && (authorization == request.headers.end() || authorization->second != "Bearer " + token_)) {
                respond(client, 401, "Unauthorized", "text/plain; charset=utf-8", "unauthorized");
                closesocket(client);
                continue;
            }
            if (request.method == "GET" && request.target == "/") {
                std::string html(page);
                const auto theme = theme_ ? theme_() : std::pair<std::string, std::string>{};
                std::string extension;
                if (!theme.first.empty()) extension += "<style id=\"user-theme\">\n" +
                    protect_raw_text_close(theme.first, "</style") + "\n</style>";
                if (!theme.second.empty()) extension += "<script id=\"user-theme-script\">\n" +
                    protect_raw_text_close(theme.second, "</script") + "\n</script>";
                const size_t insertion = html.find("<!--VCAM_THEME-->");
                if (insertion != std::string::npos) html.replace(insertion, 17, extension);
                respond(client, 200, "OK", "text/html; charset=utf-8", html,
                    "Content-Security-Policy: default-src 'self'; img-src 'self' data:; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n");
            }
            else if (request.method == "GET" && request.target == "/api/status") {
                const std::string body = status_();
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
            }
            else if (request.method == "GET" && request.target == "/api/theme") {
                const auto theme = theme_ ? theme_() : std::pair<std::string, std::string>{};
                const std::string body = "{\"css\":\"" + json_escape(theme.first) +
                    "\",\"js\":\"" + json_escape(theme.second) + "\"}";
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
            }
            else if (request.method == "GET" && request.target == "/api/logs") {
                const std::string body = "{\"text\":\"" + json_escape(logs_ ? logs_() : std::string{}) + "\"}";
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
            }
            else if (request.method == "GET" && request.target == "/api/window") {
                const std::string body = std::string("{\"hidden\":") +
                    (window_hidden_ && window_hidden_() ? "true}" : "false}");
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
            }
            else if (request.method == "POST" && request.target == "/api/window") {
                std::map<std::string, std::string> fields;
                bool hidden = false;
                if (form_decode(request.body, fields) && parse_bool(fields, "hidden", hidden) && set_window_hidden_) {
                    const bool success = set_window_hidden_(hidden);
                    const std::string body = success ?
                        (std::string("{\"ok\":true,\"hidden\":") + (hidden ? "true}" : "false}")) :
                        "{\"ok\":false,\"error\":\"tray-unavailable\"}";
                    respond(client, success ? 200 : 400, success ? "OK" : "Bad Request",
                        "application/json; charset=utf-8", body);
                } else respond(client, 400, "Bad Request", "application/json; charset=utf-8",
                    "{\"ok\":false,\"error\":\"invalid-window-setting\"}");
            }
            else if (request.method == "GET" && request.target == "/api/network") {
                const std::string body = std::string("{\"enabled\":") +
                    (network_access_.load() ? "true}" : "false}");
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
            }
            else if (request.method == "POST" && request.target == "/api/network") {
                std::map<std::string, std::string> fields;
                bool enabled = false;
                if (form_decode(request.body, fields) && parse_bool(fields, "enabled", enabled)) {
                    std::string error;
                    const bool success = rebind_network_access(enabled, error);
                    const std::string body = success ?
                        (std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true}" : "false}")) :
                        ("{\"ok\":false,\"error\":\"" + json_escape(error.empty() ? "network-rebind-failed" : error) + "\"}");
                    respond(client, success ? 200 : 400, success ? "OK" : "Bad Request",
                        "application/json; charset=utf-8", body);
                } else respond(client, 400, "Bad Request", "application/json; charset=utf-8",
                    "{\"ok\":false,\"error\":\"invalid-network-setting\"}");
            }
            else if (request.method == "GET" && request.target.rfind("/api/frame?camera=", 0) == 0) {
                int camera = 0;
                const std::string number = request.target.substr(18);
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), camera);
                const auto frame = parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()
                    ? frame_(camera) : std::nullopt;
                if (!frame || frame->yuy2.empty()) respond(client, 404, "Not Found", "text/plain", "frame unavailable");
                else {
                    const std::string extra = "X-Frame-Width: " + std::to_string(frame->size.width) +
                        "\r\nX-Frame-Height: " + std::to_string(frame->size.height) + "\r\n";
                    respond(client, 200, "OK", "application/octet-stream", std::string(frame->yuy2.begin(), frame->yuy2.end()), extra);
                }
            }
            else if (request.method == "POST" && request.target == "/api/config") {
                std::map<std::string, std::string> fields;
                WebUpdate update;
                bool valid = form_decode(request.body, fields) &&
                    parse_integer(fields, "camera", update.camera) &&
                    parse_integer(fields, "width", update.settings.resolution.width) &&
                    parse_integer(fields, "height", update.settings.resolution.height) &&
                    fields.count("rgb_pipe") && fields.count("control_pipe") && fields.count("control_pipe_access");
                const bool anyCameraOption = fields.count("fps") || fields.count("vid") || fields.count("pid") ||
                    fields.count("manufacturer") || fields.count("name") || fields.count("yuy2") || fields.count("mjpeg");
                if (valid && anyCameraOption) {
                    update.has_camera_settings = parse_integer(fields, "fps", update.settings.fps) &&
                        parse_hex_id(fields, "vid", update.settings.vid) && parse_hex_id(fields, "pid", update.settings.pid) &&
                        fields.count("manufacturer") && fields.count("name") &&
                        parse_bool(fields, "yuy2", update.settings.yuy2) && parse_bool(fields, "mjpeg", update.settings.mjpeg);
                    valid = update.has_camera_settings;
                    if (valid) {
                        update.settings.manufacturer = fields["manufacturer"];
                        update.settings.name = fields["name"];
                    }
                }
                const bool anyTheme = fields.count("custom_css") || fields.count("custom_js");
                if (valid && anyTheme) {
                    update.has_theme = fields.count("custom_css") && fields.count("custom_js");
                    valid = update.has_theme && fields["custom_css"].size() <= 65536 && fields["custom_js"].size() <= 65536;
                    if (valid) {
                        update.custom_css = fields["custom_css"];
                        update.custom_js = fields["custom_js"];
                    }
                }
                if (valid) {
                    update.rgb_pipe = fields["rgb_pipe"];
                    update.control_pipe = fields["control_pipe"];
                    if (fields["control_pipe_access"] == "owner") update.control_access = PipeAccess::owner;
                    else if (fields["control_pipe_access"] == "users") update.control_access = PipeAccess::local_users;
                    else if (fields["control_pipe_access"] == "everyone") update.control_access = PipeAccess::everyone;
                    else valid = false;
                }
                if (valid && valid_output(update.settings.resolution) && update.camera >= 1 && update.camera <= 2 &&
                    valid_pipe_path(update.rgb_pipe) && valid_pipe_path(update.control_pipe) &&
                    update.rgb_pipe != update.control_pipe) {
                    std::string error;
                    bool success = false;
                    try { success = update_(update, error); }
                    catch (...) { error = "update-failed"; }
                    const std::string body = success ? "{\"ok\":true}" :
                        "{\"ok\":false,\"error\":\"" + json_escape(error.empty() ? "update-failed" : error) + "\"}";
                    respond(client, success ? 200 : 400, success ? "OK" : "Bad Request", "application/json; charset=utf-8", body);
                } else respond(client, 400, "Bad Request", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"invalid-settings\"}");
            }
            else respond(client, 404, "Not Found", "text/plain; charset=utf-8", "not found");
            closesocket(client);
        }
    } catch (...) {}
    running_.store(false);
}

std::string create_web_token() {
    unsigned char bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Unable to create Web UI access token");
    static constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (unsigned char value : bytes) { token.push_back(digits[value >> 4]); token.push_back(digits[value & 15]); }
    return token;
}
}
