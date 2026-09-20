import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import vm from 'node:vm';

const controller = new AbortController();
let created = 0, beginRender, releaseImage;
const rendering = new Promise(resolve => { beginRender = resolve; });
const delayedImage = new Promise(resolve => { releaseImage = resolve; });
const parsed = {
    blobToURL() { ++created; return 'blob:late-image'; },
    loadDocumentImage() {}, loadNumberingImage() {},
};
class RelationshipParser {
    constructor() { this.events = {}; }
    on(name, callback) { this.events[name] = callback; }
    write() {
        this.events.opentag({ local: 'Relationship', attributes: {
            type: { local: 'Type', value: 'test/officeDocument' },
            target: { local: 'Target', value: 'word/document.xml' },
        } });
        return this;
    }
    close() {}
}
const dependencies = {
    jszip: { default: { loadAsync: async () => ({ files: {}, file: () => ({ async: async () => '' }) }) } },
    saxes: { SaxesParser: RelationshipParser },
    './document-projection.js': { projectDocumentBody: () => ({ truncated: false }) },
    'docx-preview': {
        parseAsync: async () => parsed,
        renderDocument: async document => {
            beginRender();
            await delayedImage;
            // docx-preview settles individual resource failures before returning the DOM.
            await Promise.allSettled([Promise.resolve().then(() => document.blobToURL(new Blob(['image']), 'image.png'))]);
            return [];
        },
    },
};
const context = vm.createContext({ URL, Blob, Set, Promise, Error });
const module = new vm.SourceTextModule(await fs.readFile(new URL('../../src/Glance.Components/Office/web/document-preview.js', import.meta.url), 'utf8'), { context });
await module.link(async name => {
    const exports = dependencies[name];
    assert(exports, `Unexpected dependency: ${name}`);
    return new vm.SyntheticModule(Object.keys(exports), function () {
        for (const [key, value] of Object.entries(exports)) this.setExport(key, value);
    }, { context });
});
await module.evaluate();
let published = false;
const pending = module.namespace.renderDocumentPreview(new Uint8Array(0), {
    replaceChildren() { published = true; },
}, { signal: controller.signal });
const rejected = assert.rejects(pending, error => error.name === 'AbortError');
await rendering;
controller.abort();
releaseImage();
await rejected;
assert.equal(created, 0, 'A late image must not allocate a Blob URL after preview cancellation');
assert.equal(published, false, 'Cancelled rendering must not publish DOM nodes');
console.log('Office document cancellation regression tests passed');
