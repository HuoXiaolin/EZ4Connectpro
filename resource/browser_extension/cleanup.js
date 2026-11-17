const extension = globalThis.browser || chrome;
const begin = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/([a-f0-9]{32})\/begin$/;
const returned = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/[a-f0-9]{32}\/return(?:#|$)/;

extension.runtime.onMessage.addListener((message, sender, respond) => {
    if (message.type === "sso-resources" && returned.test(sender.url || "")
        && sender.frameId === 0 && sender.tab?.id > 0) {
        openResources(sender).then(url => respond({url}), () => respond({error: true}));
        return true;
    }
    const match = begin.exec(sender.url || "");
    if (message.type !== "sso-begin" || !match || sender.frameId !== 0 || !(sender.tab?.id > 0)) {
        respond(false);
        return false;
    }
    const key = match[1];
    extension.storage.session.set({[key]: {begin: sender.url, tabId: sender.tab.id}}).then(() => {
        respond(true);
    }, () => respond(false));
    return true;
});

function removeRule(tabId) {
    extension.declarativeNetRequest.updateSessionRules({removeRuleIds: [tabId]}).catch(() => {});
    extension.alarms.clear("sso-" + tabId);
}

extension.tabs.onRemoved.addListener(tabId => {
    removeRule(tabId);
    extension.storage.session.remove("portal-" + tabId);
});
extension.tabs.onUpdated.addListener((tabId, change) => {
    if (change.url && returned.test(change.url)) {
        removeRule(tabId);
    }
});

async function openResources(sender) {
    const key = "portal-" + sender.tab.id;
    const data = (await extension.storage.session.get(key))[key];
    const current = new URL(sender.url);
    current.hash = "";
    if (!data || data.returned !== current.href || data.expires < Date.now()) throw new Error("Session expired");
    const expected = new URL(data.gateway);
    if (!["http:", "https:"].includes(expected.protocol) || expected.username || expected.password) throw new Error("Invalid gateway");
    // Only the tab authorized for this gateway may receive the core's session.
    // Nothing is stored in extension storage except the origin and random local bridge URL.
    for (let attempt = 0; attempt < 180; ++attempt) {
        const response = await fetch(data.returned.replace(/\/return$/, "/resources"), {
            credentials: "omit", redirect: "error", signal: AbortSignal.timeout(5000)
        });
        if (response.status === 202) {
            await new Promise(resolve => setTimeout(resolve, 500));
            continue;
        }
        if (!response.ok) throw new Error("Connection ended");
        const payload = await response.json();
        const portal = new URL(payload.url);
        if (portal.origin !== expected.origin || portal.pathname !== "/portal/" || portal.search || portal.hash
            || !Array.isArray(payload.cookies) || payload.cookies.length > 512) throw new Error("Invalid resource session");
        for (const cookie of payload.cookies) {
            if (new URL(cookie.url).origin !== expected.origin || !/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(cookie.name)
                || typeof cookie.value !== "string" || cookie.value.length > 16384 || /[\r\n\0]/.test(cookie.value)) {
                throw new Error("Invalid resource cookie");
            }
        }
        for (const cookie of payload.cookies) {
            await extension.cookies.set({url: expected.origin + "/", name: cookie.name, value: cookie.value,
                path: "/", secure: expected.protocol === "https:", httpOnly: true, sameSite: "lax"});
        }
        await extension.storage.session.remove(key);
        return portal.href;
    }
    throw new Error("Connection timed out");
}
extension.alarms.onAlarm.addListener(alarm => {
    if (alarm.name.startsWith("sso-")) {
        removeRule(Number(alarm.name.slice(4)));
    }
});
