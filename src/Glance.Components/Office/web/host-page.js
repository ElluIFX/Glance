const cancellation = new AbortController();
const requests = new Map();
const queued = [];
let nextRequest = 1;

function pumpRequests() {
    while (requests.size < 2 && queued.length && !cancellation.signal.aborted) {
        const request = queued.shift();
        if (request.signal.aborted) continue;
        requests.set(request.id, request);
        try { chrome.webview.postMessage(`${request.kind}:${request.id}:${request.payload}`); }
        catch (error) { requests.delete(request.id); request.reject(error); }
    }
}

function encode(bytes) {
    let text = '';
    for (let offset = 0; offset < bytes.length; offset += 16384) {
        text += String.fromCharCode(...bytes.subarray(offset, offset + 16384));
    }
    return btoa(text);
}

function requestBytes(kind, payload, signal) {
    signal.throwIfAborted();
    return new Promise((resolve, reject) => {
        const id = nextRequest++;
        const abort = () => { reject(signal.reason); pumpRequests(); };
        queued.push({
            id, kind, payload, signal,
            resolve: data => { signal.removeEventListener('abort', abort); resolve(data); },
            reject: error => { signal.removeEventListener('abort', abort); reject(error); },
        });
        signal.addEventListener('abort', abort, { once: true });
        pumpRequests();
    });
}

function convertImage(bytes, extension, signal) {
    return requestBytes('image', encode(bytes), signal);
}

chrome.webview.addEventListener('message', event => {
    const message = event.data;
    if (typeof message !== 'string') return;
    if (message.startsWith('theme:')) {
        document.body.classList.toggle('dark', message === 'theme:dark');
    } else if (message.startsWith('image:')) {
        const separator = message.indexOf(':', 6);
        const id = Number(message.slice(6, separator));
        const request = requests.get(id);
        if (!request) return;
        requests.delete(id);
        try {
            const payload = message.slice(separator + 1);
            if (!payload) throw new Error('Image conversion failed');
            request.resolve(Uint8Array.from(atob(payload), value => value.charCodeAt(0)));
        } catch (error) { request.reject(error); }
        finally { pumpRequests(); }
    }
});

chrome.webview.addEventListener('sharedbufferreceived', async event => {
    const bytes = event.getBuffer();
    let retained = false;
    let released = false;
    const release = () => {
        if (released) return;
        released = true;
        cancellation.signal.removeEventListener('abort', release);
        chrome.webview.releaseBuffer(bytes);
    };
    const reportError = error => {
        if (!cancellation.signal.aborted) chrome.webview.postMessage(`error:${error.name}:${error.message}`);
    };
    try {
        const presentation = event.additionalData?.format === 'pptx';
        document.documentElement.classList.toggle('presentation', presentation);
        const workbook = event.additionalData?.format === 'xlsx';
        const renderer = presentation
            ? (await import('./presentation-preview.mjs')).renderPresentationPreview
            : workbook ? (await import('./workbook-preview.mjs')).renderWorkbookPreview
            : (await import('./document-preview.mjs')).renderDocumentPreview;
        await renderer(bytes, document.querySelector('#document'), {
            signal: cancellation.signal,
            convertImage,
            onFirstReady: () => chrome.webview.postMessage('ready'),
            onError: reportError,
            onSourceConsumed: release,
        });
        // Lazy slide/media parsing keeps references to the original ZIP bytes.
        if (presentation && !cancellation.signal.aborted) {
            retained = true;
            cancellation.signal.addEventListener('abort', release, { once: true });
        }
        chrome.webview.postMessage('complete');
    } catch (error) { reportError(error); }
    finally { if (!retained) release(); }
});
window.addEventListener('pagehide', () => cancellation.abort(), { once: true });
chrome.webview.postMessage('source');
