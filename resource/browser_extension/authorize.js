const extension = globalThis.browser || chrome;
const rules = extension.declarativeNetRequest;
const statusLine = document.getElementById("status");
const allowButton = document.getElementById("allow");
const beginPattern = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/[a-f0-9]{32}\/begin$/;

async function authorize() {
    const key = location.hash.slice(1);
    history.replaceState(null, "", location.pathname);
    if (!/^[a-f0-9]{32}$/.test(key)) {
        throw new Error("Invalid session");
    }
    const data = await extension.storage.session.get(key);
    await extension.storage.session.remove(key);
    const {begin, tabId} = data[key] || {};
    if (!beginPattern.test(begin)) {
        throw new Error("Invalid session");
    }
    const response = await fetch(begin.replace(/\/begin$/, "/metadata"), {
        credentials: "omit",
        redirect: "error",
        signal: AbortSignal.timeout(5000)
    });
    if (!response.ok) {
        throw new Error("Session expired");
    }
    const metadata = await response.json();
    const gateway = new URL(metadata.gateway);
    const login = new URL(metadata.login);
    if ([gateway, login].some(url => !["http:", "https:"].includes(url.protocol) || url.username || url.password)) {
        throw new Error("Invalid VPN origin");
    }
    if (!Number.isInteger(tabId) || tabId < 1) {
        throw new Error("Missing login tab");
    }

    const permission = {origins: [gateway.protocol + "//" + gateway.hostname + "/*"]};
    const openLogin = async () => {
        const escapedOrigin = gateway.origin.replace(/[.*+?^\${}()|[\]\\]/g, "\\$&");
        const callback = "/passport/v1/auth/(?:cas\\?(?:[^#]*&)?ticket=|httpsOauth2\\?(?:[^#]*&)?code=)";
        const redirect = begin.replace(/\/begin$/, "/return") + "#\\0";
        await rules.updateSessionRules({
            removeRuleIds: [tabId],
            addRules: [{
                id: tabId,
                priority: 1,
                condition: {
                    regexFilter: "^" + escapedOrigin + callback + "[^&#]+(?:&[^#]*)?$",
                    isUrlFilterCaseSensitive: true,
                    tabIds: [tabId],
                    requestMethods: ["get"],
                    resourceTypes: ["main_frame"]
                },
                action: {type: "redirect", redirect: {regexSubstitution: redirect}}
            }]
        });
        await extension.alarms.create("sso-" + tabId, {delayInMinutes: 5});
        location.replace(login.href);
    };
    if (await extension.permissions.contains(permission)) {
        await openLogin();
        return;
    }
    statusLine.textContent = "允许回传扩展读取此标签页发往 " + gateway.host + " 的登录回跳。";
    allowButton.hidden = false;
    allowButton.addEventListener("click", async () => {
        allowButton.disabled = true;
        try {
            if (await extension.permissions.request(permission)) {
                await openLogin();
            } else {
                statusLine.textContent = "未获得网关权限。可以重新授权，或回工作台取消连接。";
            }
        } catch (error) {
            showFailure(error);
        } finally {
            allowButton.disabled = false;
        }
    });
}

function showFailure(error) {
    statusLine.textContent = "无法接回这次登录的票据。请回到工作台重新连接。";
    statusLine.title = error?.message || "";
}

authorize().catch(showFailure);
