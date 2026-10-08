#!/usr/bin/env node
/*
 * 页面逻辑离机测试（浏览器端 JS）。
 *
 * 为什么要有这一层：hosttest/http/ 只用 curl 打接口，验证的是 C 服务端
 * 返回的 JSON 对不对——页面里的 JS 一行都没被执行过。「按钮被 load() 整表重建冲掉」这类问题就是这样漏的：
 * 服务端老老实实返回了 holders / hint / release_url，页面却因为
 * 「renderHolders() 之后紧跟 load()，列表被 innerHTML 重建把按钮冲掉」，
 * 真机上永远看不到「解除占用并卸载」。接口测试全绿，功能照样不可用。
 *
 * 做法：把 web/index.html 里的 <script> 原文抽出来，喂给一个极简 DOM
 * （够用即可：createElement / appendChild / innerHTML / querySelector /
 *  XMLHttpRequest 同步桩），然后像用户一样点按钮，断言 DOM 里真的出现了
 * 那颗按钮、真的列出了占用者。
 *
 * 用法：node page_test.js <index.html 路径> [版本号]
 *       版本号由 run.sh 从 usbmanage.c 的 #define VERSION 现读后传入；
 *       不传则断言退化为对字面量比较（此时会打印提醒）。
 * 退出码：0 = 全过，1 = 有失败。
 */
'use strict';

var fs = require('fs');
var vm = require('vm');

var htmlPath = process.argv[2];
if (!htmlPath) {
    console.error('usage: node page_test.js <path/to/index.html> [version]');
    process.exit(2);
}
var VER = process.argv[3] || '';
if (!VER) {
    console.error('WARN: 未传版本号，页脚版本断言将退化为字面量比较（应由 run.sh 传入）');
}
var html = fs.readFileSync(htmlPath, 'utf8');
var m = /<script>([\s\S]*?)<\/script>/.exec(html);
if (!m) {
    console.error('FATAL: ' + htmlPath + ' 里找不到 <script> 块');
    process.exit(2);
}
var SRC = m[1];

var pass = 0, fail = 0;
function ok(cond, msg) {
    if (cond) { pass++; console.log('  ok    ' + msg); }
    else { fail++; console.log('  FAIL  ' + msg); }
}
function section(t) { console.log('\n' + t); }

/* ===================== 极简 DOM ===================== */

function El(tag) {
    this.tagName = String(tag).toUpperCase();
    this.attrs = {};
    this.children = [];
    this.className = '';
    this._text = '';
    this.parentNode = null;
    this.style = {};
    this.disabled = false;
    this.checked = false;
    this.offsetHeight = 0;
}
El.prototype.setAttribute = function (k, v) { this.attrs[k] = String(v); };
El.prototype.getAttribute = function (k) {
    return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null;
};
El.prototype.appendChild = function (c) { c.parentNode = this; this.children.push(c); return c; };
El.prototype.removeChild = function (c) {
    var i = this.children.indexOf(c);
    if (i >= 0) this.children.splice(i, 1);
    c.parentNode = null;
};
El.prototype.addEventListener = function () { };
El.prototype.querySelector = function (sel) {
    var r = [];
    walk(this, function (e) { if (match(e, sel)) r.push(e); });
    return r.length ? r[0] : null;
};
El.prototype.querySelectorAll = function (sel) {
    var r = [];
    walk(this, function (e) { if (match(e, sel)) r.push(e); });
    return r;
};
Object.defineProperty(El.prototype, 'textContent', {
    /* 递归取文本：宿主元素里套着 pid/进程名/路径几层 div，断言要能读到 */
    get: function () {
        var s = this._text;
        for (var i = 0; i < this.children.length; i++) {
            if (this.children[i] instanceof El) s += this.children[i].textContent;
        }
        return s;
    },
    set: function (v) { this._text = String(v); this.children = []; }
});
Object.defineProperty(El.prototype, 'innerHTML', {
    get: function () { return this._text; },
    set: function (h) { this.children = []; this._text = ''; parseInto(String(h), this); }
});

function walk(root, fn) {
    for (var i = 0; i < root.children.length; i++) {
        var c = root.children[i];
        if (c instanceof El) { fn(c); walk(c, fn); }
    }
}
function hasClass(el, c) {
    var p = String(el.className || '').split(/\s+/);
    for (var i = 0; i < p.length; i++) if (p[i] === c) return true;
    return false;
}
function match(el, sel) {
    var m;
    if ((m = /^\.([\w-]+)$/.exec(sel))) return hasClass(el, m[1]);
    if ((m = /^([\w-]*)\[([\w-]+)(?:="([^"]*)")?\]$/.exec(sel))) {
        if (m[1] && el.tagName !== m[1].toUpperCase()) return false;
        var v = el.getAttribute(m[2]);
        if (m[3] === undefined) return v !== null;
        return v === m[3];
    }
    if ((m = /^([\w-]+)$/.exec(sel))) return el.tagName === m[1].toUpperCase();
    return false;
}

var VOID = { input: 1, br: 1, hr: 1, img: 1, meta: 1, link: 1 };
function parseInto(h, root) {
    var i = 0, n = h.length, stack = [root];
    function text(s) {
        var top = stack[stack.length - 1];
        if (top instanceof El && top !== root) top._text += s;
    }
    while (i < n) {
        var lt = h.indexOf('<', i);
        if (lt < 0) { text(h.slice(i)); break; }
        if (lt > i) text(h.slice(i, lt));
        var gt = h.indexOf('>', lt);
        if (gt < 0) { text(h.slice(lt)); break; }
        var raw = h.slice(lt + 1, gt).trim();
        i = gt + 1;
        if (raw.charAt(0) === '/') { if (stack.length > 1) stack.pop(); continue; }
        var selfClose = /\/$/.test(raw);
        if (selfClose) raw = raw.replace(/\/$/, '').trim();
        var mm = /^([a-zA-Z0-9]+)\s*([\s\S]*)$/.exec(raw);
        if (!mm) continue;
        var el = new El(mm[1]), rest = mm[2], a, re = /([\w-]+)\s*=\s*"([^"]*)"/g;
        while ((a = re.exec(rest))) el.attrs[a[1]] = a[2];
        if (el.attrs['class'] !== undefined) el.className = el.attrs['class'];
        if (/(^|\s)disabled(\s|$)/.test(rest)) el.disabled = true;
        var top = stack[stack.length - 1];
        top.children.push(el);
        el.parentNode = top;
        if (!selfClose && !VOID[mm[1].toLowerCase()]) stack.push(el);
    }
}

/* 页面里用到的 id 都在这里注册。
   后半段那些是为了中英双语、从静态 HTML 改成由 JS 填写的文案节点。 */
var IDS = ['list', 'msg', 'status', 'diag', 'refresh', 'shutdown', 'ver', 'host', 'preview',
           'appname', 'tagline', 'lblDiag', 'appnameFoot', 'hostLbl',
           'footA', 'footB', 'footC'];
function makeDoc(search) {
    var d = {
        els: {}, _roots: [],
        body: new El('body'),
        documentElement: new El('html'),
        location: { host: '192.168.1.100:9100', protocol: 'http:', search: search || '',
                    href: 'http://192.168.1.100:9100/' + (search || '') },
        createElement: function (t) { return new El(t); },
        getElementById: function (x) { return d.els[x] || null; },
        querySelector: function (sel) {
            for (var i = 0; i < d._roots.length; i++) {
                var r = d._roots[i].querySelector(sel);
                if (r) return r;
            }
            return null;
        },
        querySelectorAll: function (sel) {
            var out = [];
            for (var i = 0; i < d._roots.length; i++) out = out.concat(d._roots[i].querySelectorAll(sel));
            return out;
        },
        addEventListener: function () { }
    };
    IDS.forEach(function (x) {
        var tag = (x === 'diag') ? 'input' : 'div';
        d.els[x] = new El(tag);
        d._roots.push(d.els[x]);
    });
    /* 顶部提示条是 fixed 定位，页面按它的实际高度给 body 让位。
       桩里给个非零高度，才能断言"让位"这件事真的发生了。 */
    d.els['msg'].offsetHeight = 42;
    return d;
}

/* ===================== 假服务端 ===================== */

var state = { vols: [] };
var routes = {};
/* 最近一次 XHR 的 URL 与请求头 —— 用来断言"页面确实声明了自定义头" */
var lastRequest = null;

function route(url) {
    var best = null;
    Object.keys(routes).forEach(function (k) {
        if (url.indexOf(k) === 0 && (best === null || k.length > best.length)) best = k;
    });
    if (best === null) return null;
    var v = routes[best];
    return typeof v === 'function' ? v(url) : v;
}

function FakeXHR() { }
FakeXHR.prototype.open = function (mth, u) { this._url = u; this._headers = {}; this.readyState = 1; };
FakeXHR.prototype.setRequestHeader = function (k, v) { this._headers[k] = String(v); };
FakeXHR.prototype.send = function () {
    lastRequest = { url: this._url, headers: this._headers };
    var r = route(this._url) || { status: 404, body: '{}' };
    this.status = r.status;
    this.responseText = r.body;
    this.readyState = 4;
    if (typeof this.onreadystatechange === 'function') this.onreadystatechange();
};

/* ===================== 装配并执行页面脚本 ===================== */

var VOL = {
    mount: '/mnt/usb0', fstype: 'exfatfs', device: '/dev/da2p1',
    total: 2097152000000, free: 124800000000, ejectable: true
};
var EJECT_FAIL = {
    ok: false, code: 16, mount: '/mnt/usb0', forced: false, busy: true,
    msg: '卸载失败：Device busy。未查到占用进程（多为系统沙箱挂接），点「解除占用并卸载」强制处理',
    holders_supported: true, holders: [], holder_count: 0,
    hint: '没有进程打开这块盘上的文件，占用大概率来自系统沙箱的 nullfs 挂接（应用把盘里的目录挂进了自己的视图）；解除占用会先卸掉这些挂接再强制卸载',
    release_url: '/release?mount=%2Fmnt%2Fusb0&confirm=1'
};

state.vols = [VOL];
routes['/version'] = { status: 200, body: JSON.stringify({ version: VER || '1.0.0' }) };
routes['/list'] = function () {
    var v = state.vols.slice();
    return { status: 200, body: JSON.stringify({ count: v.length, scope: 'usb', volumes: v, shown: v.length, truncated: false }) };
};
routes['/eject'] = { status: 200, body: JSON.stringify(EJECT_FAIL) };

/* 装配一份页面运行环境。
 *
 * 语言只靠 navigator.language 注入 —— 页面就是这么判语言的，所以这一份桩
 * 同时也是"页面能跟随系统语言"的验证手段：
 *   makeCtx('zh-CN')   -> 中文界面
 *   makeCtx('en-US')   -> 英文界面
 *   makeCtx('zh-CN','?lang=en') -> 地址栏上的 ?lang= 覆盖 navigator
 * 每次都是全新的 DOM 与上下文，互不串味。 */
function makeCtx(navLang, search) {
    var doc = makeDoc(search);
    var sandbox = {
        document: doc,
        window: { scrollTo: function () { } },
        navigator: { language: navLang },
        XMLHttpRequest: FakeXHR,
        /* confirm 一律同意，但把文案记下来——"卸错盘"就靠确认框里的卷标来防 */
        confirm: function (msg) { sandbox.lastConfirm = msg; return true; },
        setTimeout: function () { return 0; },      /* 关掉 ok 提示条的 7s 自动收起，测试要看到它 */
        console: console
    };
    vm.createContext(sandbox);
    vm.runInContext(SRC, sandbox, { filename: 'page.js' });
    return { doc: doc, sandbox: sandbox };
}

var ctx = makeCtx('zh-CN');
var doc = ctx.doc;
var sandbox = ctx.sandbox;

section('A. 页面脚本能装配执行');
ok(typeof sandbox.doEject === 'function', '定义了 doEject');
ok(typeof sandbox.doRelease === 'function', '定义了 doRelease');
ok(typeof sandbox.applyPending === 'function', '定义了 applyPending（load() 重绘后重挂就地反馈）');

section('B. 初始列表');
var list = doc.getElementById('list');
var ejectBtn = list.querySelector('button[data-eject]');
ok(!!ejectBtn, '渲染出弹出按钮');
ok(ejectBtn && ejectBtn.getAttribute('data-eject') === '/mnt/usb0', '按钮指向 /mnt/usb0');
ok(ejectBtn && ejectBtn.textContent === '安全弹出 /mnt/usb0', '按钮文案正确');
ok(doc.getElementById('ver').textContent === 'v' + (VER || '1.0.0'), '页脚版本号来自 /version');

section('C. 卸载失败（EBUSY、查不到占用者）——必须就地给出解除入口');
sandbox.clearPending();
sandbox.doEject(ejectBtn);

var card = doc.querySelector('div[data-mount="/mnt/usb0"]');
var rel = card ? card.querySelector('button[data-release]') : null;
ok(!!rel, '卡片上出现「解除占用并卸载」按钮');
ok(rel && rel.getAttribute('data-release') === '/mnt/usb0', '解除按钮带正确的挂载点');
ok(rel && rel.textContent === '解除占用并卸载', '解除按钮文案正确');
ok(card && hasClass(card, 'fail'), '卡片标记为 fail（红框）');
ok(!!(card && card.querySelector('.holders')), '卡片上有「谁在占用」区域');
ok(card && /没有进程打开这块盘上的文件/.test(card.querySelector('.holders').textContent),
    '无占用者时给出说明（多为沙箱 nullfs 挂接）');
ok(/卸载失败/.test(doc.getElementById('msg').textContent), '顶部提示条报出失败');
ok(/mnt\/usb0/.test(doc.getElementById('msg').textContent), '顶部提示条指明是哪个盘');
ok(doc.body.style.paddingTop === '42px', '提示条出现时给 body 让位，不再盖住标题');

section('D. 点「解除占用并卸载」成功后');
var released = null;
routes['/release'] = function (u) {
    released = u;
    return {
        status: 200, body: JSON.stringify({
            unmounted: true, mount: '/mnt/usb0', killed: [], skipped: 0, after: 0, still: [],
            child_released: [{ path: '/mnt/sandbox/ABCD_000', reason: 'released' }],
            child_released_count: 1, forced_retry: true,
            msg: '已解除占用并卸载（已解除系统内部挂接），现在可以安全拔出了'
        })
    };
};
state.vols = [];                       /* 模拟卸载成功后 /list 不再返回该卷 */
sandbox.doRelease(rel);
ok(released === '/release?mount=%2Fmnt%2Fusb0&confirm=1', '确实打了带 confirm=1 的 /release');
ok(/已解除占用并卸载/.test(doc.getElementById('msg').textContent), '顶部提示条报成功');
ok(doc.querySelector('div[data-mount="/mnt/usb0"]') === null, '卸载后列表里不再有该卷');
ok(doc.body.style.paddingTop === '42px', '成功提示条同样给 body 让位');

section('E. 解除之后仍然卸不掉——必须保留重试按钮 + 列出残留占用者');
state.vols = [VOL];
sandbox.clearPending();
sandbox.load();
var btn2 = doc.getElementById('list').querySelector('button[data-eject]');
sandbox.doEject(btn2);
var rel2 = doc.querySelector('div[data-mount="/mnt/usb0"]').querySelector('button[data-release]');
ok(!!rel2, '（前置）再次失败后又有了解除按钮');
routes['/release'] = {
    status: 200, body: JSON.stringify({
        unmounted: false, mount: '/mnt/usb0', killed: [], skipped: 1, after: 1, forced_retry: true,
        child_released: [], child_released_count: 0,
        still: [{ pid: 123, comm: 'SceShellCore', fd: 9, kind: 'file', path: '/mnt/usb0/a.mp4', killable: false, why: '系统进程，已跳过' }],
        msg: '解除占用后仍无法卸载：Device busy（仍有进程占用，见 still）'
    })
};
sandbox.doRelease(rel2);
var card3 = doc.querySelector('div[data-mount="/mnt/usb0"]');
ok(!!(card3 && card3.querySelector('button[data-release]')), '仍失败时保留「解除占用并卸载」重试按钮');
ok(!!(card3 && card3.querySelector('.holders')), '仍失败时列出仍在占用的进程');
ok(card3 && /SceShellCore/.test(card3.querySelector('.holders').textContent), '名单里读得到进程名');
ok(card3 && /a\.mp4/.test(card3.querySelector('.holders').textContent), '名单里读得到被占用的文件');
ok(/解除占用/.test(doc.getElementById('msg').textContent), '顶部提示条报出解除后仍失败');

section('F. 反证：旧写法（renderHolders 之后紧跟 load()）必然丢掉按钮');
state.vols = [VOL];
sandbox.clearPending();
sandbox.load();
var c = doc.querySelector('div[data-mount="/mnt/usb0"]');
sandbox.renderHolders(c, [], 'hint', false);
ok(!!doc.querySelector('button[data-release]'), '（前置）刚 append 时按钮确实在');
sandbox.load();                        /* 若在 load() 之前 append，按钮会在这里被冲掉 */
ok(!doc.querySelector('button[data-release]'),
    '紧接 load() 重建列表后按钮消失 —— 本用例真能抓到这个 bug');

section('G. 卷标显示 —— 光看设备号分不清哪块是哪个');
var VOL_LABEL = {
    mount: '/mnt/usb1', fstype: 'exfatfs', device: '/dev/da1s1',
    total: 1000000000, free: 500000000, ejectable: true, label: '移动硬盘'
};
state.vols = [VOL, VOL_LABEL];
sandbox.clearPending();
sandbox.load();
var c0 = doc.querySelector('div[data-mount="/mnt/usb0"]');
var c1 = doc.querySelector('div[data-mount="/mnt/usb1"]');
ok(!!(c1 && /卷标/.test(c1.textContent) && /移动硬盘/.test(c1.textContent)),
    '有卷标的卡片显示「卷标 移动硬盘」');
ok(!!(c0 && /没有卷标/.test(c0.textContent)),
    '没有卷标的卡片明说"没有卷标"，不拿设备名糊弄');
ok(!!(c1 && /exfatfs/.test(c1.textContent) && /da1s1/.test(c1.textContent)),
    '设备号与文件系统仍然照常显示');

/* 这一步才是真正防"卸错盘"的地方：确认框必须点名卷标 */
sandbox.doEject(c1.querySelector('button[data-eject]'));
ok(/卷标 移动硬盘/.test(sandbox.lastConfirm || ''), '弹出确认框里点名了卷标');
ok(/\/mnt\/usb1/.test(sandbox.lastConfirm || ''), '弹出确认框里点名了挂载点');
state.vols = [VOL];
sandbox.clearPending();
sandbox.load();
sandbox.doEject(doc.querySelector('div[data-mount="/mnt/usb0"]').querySelector('button[data-eject]'));
ok(/无卷标/.test(sandbox.lastConfirm || ''), '没有卷标时确认框写「无卷标」，不留空');

section('H. 「关闭服务」：把进程彻底关掉');
state.vols = [VOL];
sandbox.clearPending();
sandbox.load();
var shutdownCalled = null;
routes['/shutdown'] = function (u) {
    shutdownCalled = u;
    return {
        status: 200, body: JSON.stringify({
            ok: true, app: 'usbmanage', version: VER || '1.0.0',
            state: 'shutting_down', msg: '服务已关闭，可从越狱工具箱重新加载'
        })
    };
};
doc.getElementById('shutdown').onclick.call(doc.getElementById('shutdown'));
ok(shutdownCalled === '/shutdown', '确实打了 /shutdown');
ok(!!(lastRequest && lastRequest.headers['X-Requested-With'] === 'usbmanage'),
    '请求带 X-Requested-With: usbmanage（在电脑上点关闭才会被主机放行）');
ok(!!(lastRequest && lastRequest.headers['Accept'] === 'application/json'),
    '请求显式声明 Accept: application/json（不会被 302 回首页）');
ok(/服务已关闭/.test(doc.getElementById('list').textContent), '列表区换成「服务已关闭」');
ok(/可从越狱工具箱重新加载/.test(doc.getElementById('msg').textContent), '顶部提示条说清下一步');
ok(doc.getElementById('refresh').disabled === true &&
   doc.getElementById('shutdown').disabled === true,
    '关闭后按钮一律禁用');
doc.getElementById('refresh').onclick.call(doc.getElementById('refresh'));
ok(shutdownCalled === '/shutdown', '关闭后再点刷新也不会再发请求（页面已失效）');

section('I. 跟随系统语言：中文（简体/繁体）用对应中文，其余一律英文');
/*
 * 约定：**系统语言是中文就上中文界面，简体走简体、繁体走繁体；其余语言
 * 一律英文**。页面判语言的依据只有两个：地址上的 ?lang=、以及
 * navigator.language（它由主机系统语言决定）。这里把 navigator 换成各种
 * 语言各建一份环境，断言整套 UI 是**整体**切换的——不允许出现"标题英文、
 * 按钮中文"这种半拉子状态。
 * 简繁只差字形，标题（PS5 USB管理器）两边同字，所以繁体的断言得挑只在
 * 繁体里出现的词（重新整理清單 / 儲存 / 位址）。
 */
function zh(c) { return /[\u4e00-\u9fa5]/.test(c); }   /* 含汉字 */

state.vols = [VOL];
routes['/eject'] = { status: 200, body: JSON.stringify(EJECT_FAIL) };

var en = makeCtx('en-US');
var ed = en.doc;
ok(ed.getElementById('appname').textContent === 'PS5 USB Manager', 'en-US：标题为 PS5 USB Manager');
ok(/Sync first/.test(ed.getElementById('tagline').textContent), 'en-US：副标题已切英文');
ok(ed.getElementById('refresh').textContent === 'Refresh', 'en-US：刷新按钮已切英文');
ok(ed.getElementById('shutdown').textContent === 'Stop service', 'en-US：关闭按钮已切英文');
ok(ed.getElementById('lblDiag').textContent === 'Show internal storage (read-only)', 'en-US：复选框已切英文');
ok(ed.getElementById('footC').textContent === 'For use on your own or explicitly authorised devices only.',
   'en-US：页脚免责声明已切英文');
var enBtn = ed.getElementById('list').querySelector('button[data-eject]');
ok(!!enBtn && enBtn.textContent === 'Eject /mnt/usb0', 'en-US：弹出按钮文案为英文');
var enCard = ed.querySelector('div[data-mount="/mnt/usb0"]');
ok(!!(enCard && /no label/.test(enCard.textContent)), 'en-US：无卷标时的说明也是英文');
ok(!zh(ed.getElementById('footA').textContent) && !zh(ed.getElementById('footB').textContent),
   'en-US：英文页脚里不残留汉字（整体切换，不是半拉子）');
ok(!!(enCard && enCard.textContent.indexOf(' (free ') >= 0),
   'en-US：剩余空间用半角括号（标点也是语言的一部分）');

/* 简体 */
var hans = makeCtx('zh-CN'), hsd = hans.doc;
ok(hsd.getElementById('refresh').textContent === '刷新列表', 'zh-CN：刷新按钮是简体');
ok(hsd.getElementById('shutdown').textContent === '关闭服务', 'zh-CN：关闭按钮是简体');
ok(hsd.getElementById('footA').textContent.indexOf('主机内部存储') >= 0, 'zh-CN：页脚是简体');
var cnCard = hsd.querySelector('div[data-mount="/mnt/usb0"]');
ok(!!(cnCard && cnCard.textContent.indexOf('（剩余 ') >= 0),
   'zh-CN：剩余空间用全角括号');
ok(zh(hsd.getElementById('footA').textContent) && zh(hsd.getElementById('footB').textContent),
   'zh-CN：中文页脚仍是中文');

/* 繁体：同一套 UI 换字形，不能只是"显示中文"就算数 */
var hant = makeCtx('zh-TW'), htd = hant.doc;
ok(htd.getElementById('appname').textContent === 'PS5 USB管理器', 'zh-TW：标题为中文');
ok(htd.getElementById('refresh').textContent === '重新整理清單', 'zh-TW：刷新按钮是繁体');
ok(htd.getElementById('shutdown').textContent === '關閉服務', 'zh-TW：关闭按钮是繁体');
ok(htd.getElementById('footA').textContent.indexOf('儲存') >= 0 &&
   htd.getElementById('footA').textContent.indexOf('存储') < 0,
   'zh-TW：页脚是繁体、不残留简体');
ok(!!(htd.querySelector('div[data-mount="/mnt/usb0"]').textContent.indexOf('（剩餘 ') >= 0),
   'zh-TW：剩余空间用全角括号');

ok(ed.documentElement.lang === 'en' && hsd.documentElement.lang === 'zh-CN' &&
   htd.documentElement.lang === 'zh-Hant',
   'html lang 跟着界面语言走（影响字体回落与屏幕阅读器）');

/* 同一个语言的各种写法都要落到正确的档 */
ok(makeCtx('zh-Hans-CN').doc.getElementById('refresh').textContent === '刷新列表', 'zh-Hans-CN 走简体');
ok(makeCtx('zh').doc.getElementById('refresh').textContent === '刷新列表', '光秃秃的 zh 走简体');
ok(makeCtx('zh-HK').doc.getElementById('refresh').textContent === '重新整理清單', 'zh-HK 走繁体');
ok(makeCtx('zh-MO').doc.getElementById('refresh').textContent === '重新整理清單', 'zh-MO 走繁体');
ok(makeCtx('zh-Hant').doc.getElementById('refresh').textContent === '重新整理清單', 'zh-Hant 走繁体');
ok(makeCtx('ja-JP').doc.getElementById('refresh').textContent === 'Refresh', 'ja-JP 走英文');
ok(makeCtx('ko-KR').doc.getElementById('refresh').textContent === 'Refresh', 'ko-KR 走英文');

/* 地址上的 ?lang= 优先级最高：调试和自动化用它把语言钉死 */
ok(makeCtx('zh-CN', '?lang=en').doc.getElementById('refresh').textContent === 'Refresh',
   '?lang=en 能覆盖 navigator.language');
ok(makeCtx('en-US', '?lang=zh').doc.getElementById('refresh').textContent === '刷新列表',
   '?lang=zh 能覆盖 navigator.language');
ok(makeCtx('en-US', '?lang=zh-Hant').doc.getElementById('refresh').textContent === '重新整理清單',
   '?lang=zh-Hant 能覆盖 navigator.language（繁体的显式写法）');
ok(makeCtx('zh-TW', '?lang=zh-Hans').doc.getElementById('refresh').textContent === '刷新列表',
   '?lang=zh-Hans 能把繁体系统拉回简体');

console.log('\n通过 ' + pass + ' 项，失败 ' + fail + ' 项');
process.exit(fail ? 1 : 0);
