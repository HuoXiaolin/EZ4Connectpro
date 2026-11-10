const extension = globalThis.browser || chrome;
const begin = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/([a-f0-9]{32})\/begin$/;
const returned = /^http:\/\/127\.0\.0\.1:\d{1,5}\/ez4connectpro\/[a-f0-9]{32}\/return(?:#|$)/;

extension.runtime.onMessage.addListener((message, sender, respond) => {
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

extension.tabs.onRemoved.addListener(removeRule);
extension.tabs.onUpdated.addListener((tabId, change) => {
    if (change.url && returned.test(change.url)) {
        removeRule(tabId);
    }
});
extension.alarms.onAlarm.addListener(alarm => {
    if (alarm.name.startsWith("sso-")) {
        removeRule(Number(alarm.name.slice(4)));
    }
});
