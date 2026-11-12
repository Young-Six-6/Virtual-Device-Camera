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
constexpr size_t max_body = 4096;

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
.state{display:flex;gap:8px;align-items:center}.dot{width:8px;height:8px;border:1px solid var(--accent);border-radius:50%;display:inline-block}.dot.off{border-color:var(--bad);background:var(--bad)}.statgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:18px}.stat{border-top:1px solid var(--line);padding:10px 0}.stat b{display:block;font-weight:500;margin-top:3px}.preview{min-height:280px;border:1px solid var(--line);background:#0a0d0e;display:grid;place-items:center;overflow:hidden}.preview canvas{width:100%;height:auto;max-height:65vh;object-fit:contain}.message{min-height:22px;margin-top:12px;color:var(--accent)}.message.error{color:var(--bad)}
@media(max-width:760px){main{margin:20px auto}.layout{grid-template-columns:1fr}.top{align-items:start;gap:8px;flex-direction:column}}
</style></head><body><main>
<header class="top"><div><h1>VIRTUAL CAMERA / CONTROL</h1><div class="sub">本机设备状态与运行参数</div></div><div id="webstate" class="state"><i class="dot"></i><span>连接中</span></div></header>
<div class="layout"><section><h2>运行设置</h2>
<label for="camera">摄像头</label><select id="camera"><option value="1">摄像头 1</option><option value="2">摄像头 2</option></select>
<div class="row"><div><label for="width">宽度</label><input id="width" type="number" min="2" max="4096" step="2"></div><div><label for="height">高度</label><input id="height" type="number" min="1" max="2160"></div></div>
<label for="rgb">RGB 输入管道名</label><input id="rgb" autocomplete="off">
<label for="control">控制管道名</label><input id="control" autocomplete="off">
<label for="access">控制管道访问范围</label><select id="access"><option value="everyone">Everyone + SYSTEM</option><option value="owner">当前用户 + SYSTEM</option><option value="users">本机 Users 组 + SYSTEM</option></select>
<div class="actions"><button id="apply">应用并保存</button><button id="refresh" class="secondary">刷新状态</button></div><div id="message" class="message"></div>
</section><section><h2>设备与管道状态</h2><div class="statgrid" id="stats"></div><div class="preview"><canvas id="frame" width="640" height="480"></canvas></div><div class="sub" style="margin-top:10px">预览来自当前摄像头 YUY2 输出；管道无新帧时显示黑画面。</div></section></div>
</main><script>
const token=location.hash.slice(1);history.replaceState(null,"",location.pathname);let current=null;
const $=id=>document.getElementById(id);const api=async(path,opts={})=>fetch(path,{...opts,headers:{Authorization:`Bearer ${token}`,...(opts.headers||{})}});
function state(ok,text){$("webstate").innerHTML=`<i class="dot ${ok?"":"off"}"></i><span>${text}</span>`}
async function refresh(loadConfig=false){try{const r=await api('/api/status');if(!r.ok)throw Error('接口未授权或不可用');current=await r.json();state(true,'本机服务在线');if(loadConfig){const cam=current.cameras.find(c=>c.index===Number($("camera").value));if(cam){$("width").value=cam.width;$("height").value=cam.height}$("camera").options[1].disabled=!current.cameras[1].present;$("rgb").value=current.rgbPipe.name;$("control").value=current.controlPipe.name;$("access").value=current.controlPipeAccess}
const pipeState=!current.rgbPipe.active?'当前输入源未使用':current.rgbPipe.connected?'客户端已连接':current.rgbPipe.listening?'监听中，等待客户端连接':'监听未运行';const frameState=current.rgbPipe.frames?`${current.rgbPipe.frames} 帧 · 最近 ${Math.round(current.rgbPipe.frameAgeMs/1000)} 秒`:'尚未收到帧';
$("stats").innerHTML=`<div class="stat"><span class="muted">虚拟 USB 驱动接口</span><b>${current.driverAvailable?'已发现':'未发现'}</b></div><div class="stat"><span class="muted">RGB 输入管道</span><b>${pipeState}</b><small class="muted">${frameState}</small></div><div class="stat"><span class="muted">控制管道</span><b>${current.controlPipe.running?'监听中':'未运行'}</b></div><div class="stat"><span class="muted">Web UI</span><b>127.0.0.1:${current.webPort}</b></div>`
}catch(e){state(false,'状态读取失败');$("message").textContent=e.message}}
function clip(v){return Math.max(0,Math.min(255,v))}
function draw(bytes,w,h){const canvas=$("frame"),ctx=canvas.getContext('2d');canvas.width=w;canvas.height=h;const image=ctx.createImageData(w,h),out=image.data;for(let i=0,j=0;i<w*h*2;i+=4,j+=8){const u=bytes[i+1]-128,v=bytes[i+3]-128;for(let p=0;p<2;p++){const y=Math.max(0,bytes[i+p*2]-16),c=298*y;out[j+p*4]=clip((c+409*v+128)>>8);out[j+p*4+1]=clip((c-100*u-208*v+128)>>8);out[j+p*4+2]=clip((c+516*u+128)>>8);out[j+p*4+3]=255}}ctx.putImageData(image,0,0)}
async function preview(){try{const r=await api(`/api/frame?camera=${$("camera").value}`);if(!r.ok)return;const w=Number(r.headers.get('X-Frame-Width')),h=Number(r.headers.get('X-Frame-Height'));draw(new Uint8Array(await r.arrayBuffer()),w,h)}catch(_){}}
$("camera").onchange=()=>refresh(true);$("refresh").onclick=()=>refresh(true);$("apply").onclick=async()=>{const message=$("message");message.className='message';message.textContent='应用中…';const body=new URLSearchParams({camera:$("camera").value,width:$("width").value,height:$("height").value,rgb_pipe:$("rgb").value,control_pipe:$("control").value,control_pipe_access:$("access").value});try{const r=await api('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const result=await r.json();if(!r.ok||!result.ok)throw Error(result.error||'应用失败');message.textContent='配置已应用并写入 camera.ini。摄像头客户端需重新打开；管道发送端请连接新名称。';await refresh(true)}catch(e){message.className='message error';message.textContent=e.message}};
if(!token)state(false,'请从程序控制台打开带令牌的地址');refresh(true);setInterval(()=>{refresh();preview()},1000);preview();
</script></body></html>)HTML";

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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
std::string json_escape(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c >= 0x20) out.push_back(static_cast<char>(c));
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
                      std::function<bool(const WebUpdate&, std::string&)> update) {
    if (worker_.joinable() || token.size() < 32 || !status || !frame || !update) return false;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    winsock_initialized_.store(true);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { winsock_initialized_.store(false); WSACleanup(); return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        closesocket(listener);
        winsock_initialized_.store(false);
        WSACleanup();
        return false;
    }
    port_ = port;
    token_ = std::move(token);
    status_ = std::move(status);
    frame_ = std::move(frame);
    update_ = std::move(update);
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
    const uintptr_t value = listener_.exchange(invalid_socket);
    if (value != invalid_socket) {
        const SOCKET listener = static_cast<SOCKET>(value);
        shutdown(listener, SD_BOTH);
        closesocket(listener);
    }
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    if (winsock_initialized_.exchange(false)) WSACleanup();
}
void WebServer::run() noexcept {
    const SOCKET listener = static_cast<SOCKET>(listener_.load());
    try {
        while (!stopping_.load()) {
            sockaddr_in client_address{};
            int address_size = sizeof(client_address);
            SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&client_address), &address_size);
            if (client == INVALID_SOCKET) break;
            DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            Request request;
            if (!receive_request(client, request)) {
                respond(client, 400, "Bad Request", "text/plain; charset=utf-8", "bad request");
                closesocket(client);
                continue;
            }
            const std::string expected_host = "127.0.0.1:" + std::to_string(port_);
            const auto host = request.headers.find("host");
            const auto origin = request.headers.find("origin");
            const std::string expected_origin = "http://127.0.0.1:" + std::to_string(port_);
            if (host == request.headers.end() || host->second != expected_host ||
                (origin != request.headers.end() && origin->second != expected_origin)) {
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
                respond(client, 200, "OK", "text/html; charset=utf-8", page,
                    "Content-Security-Policy: default-src 'self'; img-src 'self' data:; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n");
            }
            else if (request.method == "GET" && request.target == "/api/status") {
                const std::string body = status_();
                respond(client, 200, "OK", "application/json; charset=utf-8", body);
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
                    parse_integer(fields, "width", update.resolution.width) &&
                    parse_integer(fields, "height", update.resolution.height) &&
                    fields.count("rgb_pipe") && fields.count("control_pipe") && fields.count("control_pipe_access");
                if (valid) {
                    update.rgb_pipe = fields["rgb_pipe"];
                    update.control_pipe = fields["control_pipe"];
                    if (fields["control_pipe_access"] == "owner") update.control_access = PipeAccess::owner;
                    else if (fields["control_pipe_access"] == "users") update.control_access = PipeAccess::local_users;
                    else if (fields["control_pipe_access"] == "everyone") update.control_access = PipeAccess::everyone;
                    else valid = false;
                }
                if (valid && valid_output(update.resolution) && update.camera >= 1 && update.camera <= 2 &&
                    valid_pipe_path(update.rgb_pipe) && valid_pipe_path(update.control_pipe) &&
                    update.rgb_pipe != update.control_pipe) {
                    std::string error;
                    const bool success = update_(update, error);
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
