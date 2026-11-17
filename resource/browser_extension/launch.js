const extension = globalThis.browser || chrome;
const begin = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/([a-f0-9]{32})\/begin$/;
const match = begin.exec(location.href);
if (match && document.querySelector('meta[name="ez4connectpro"][content="sso"]')) {
    extension.runtime.sendMessage({type: "sso-begin"}).then(ready => {
        if (ready) {
            location.replace(extension.runtime.getURL("authorize.html") + "#" + match[1]);
        }
    }).catch(() => {});
}

if (/^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/[a-f0-9]{32}\/return(?:#|$)/.test(location.href)
    && document.querySelector('meta[name="ez4connectpro"][content="resources"]')) {
    let polls = 0;
    const timer = setInterval(() => {
        if (++polls > 100) { clearInterval(timer); return; }
        if (document.documentElement.dataset.ez4Callback !== "ready") return;
        clearInterval(timer);
        extension.runtime.sendMessage({type: "sso-resources"}).then(result => {
            if (result?.url) location.replace(result.url);
            else document.getElementById("status").textContent = "资源页会话未能恢复，请回工作台检查连接状态。";
        }).catch(() => {});
    }, 100);
}
