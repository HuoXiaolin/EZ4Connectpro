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
