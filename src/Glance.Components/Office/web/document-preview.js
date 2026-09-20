import JSZip from 'jszip';
import { parseAsync, renderDocument } from 'docx-preview';
import { SaxesParser } from 'saxes';
import { projectDocumentBody } from './document-projection.js';

const renderOptions = { renderAltChunks: false, useBase64URL: false };
const nativeImageExtensions = new Set(['wmf', 'emf', 'tif', 'tiff']);

function checkCancellation(signal) {
    signal?.throwIfAborted();
}

function afterFirstPaint(signal) {
    checkCancellation(signal);
    return new Promise((resolve, reject) => {
        let frame, timer;
        const finish = error => {
            cancelAnimationFrame(frame);
            clearTimeout(timer);
            signal?.removeEventListener('abort', cancel);
            if (error) reject(error);
            else resolve();
        };
        const cancel = () => finish(signal.reason);
        signal?.addEventListener('abort', cancel, { once: true });
        frame = requestAnimationFrame(() => {
            frame = requestAnimationFrame(() => { timer = setTimeout(() => finish(), 0); });
        });
    });
}

function documentPath(relationships) {
    const parser = new SaxesParser({ xmlns: true });
    let target;
    parser.on('doctype', () => { throw new Error('Document types are not supported'); });
    parser.on('opentag', tag => {
        if (tag.local !== 'Relationship') return;
        const attributes = Object.fromEntries(Object.values(tag.attributes).map(value => [value.local, value.value]));
        if (!attributes.Type?.endsWith('/officeDocument')) return;
        if (target || attributes.TargetMode === 'External') throw new Error('Invalid main document relationship');
        target = attributes.Target;
    });
    parser.write(relationships).close();
    if (!target || target.includes('\\') || target.includes(':')) throw new Error('Invalid main document path');
    const parts = [];
    for (const part of target.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (!parts.length) throw new Error('Document path escapes package');
            parts.pop();
        } else parts.push(part);
    }
    return parts.join('/');
}

async function firstDocument(bytes, signal) {
    if (bytes.byteLength > 128 * 1024 * 1024) throw new Error('Document exceeds preview budget');
    const zip = await JSZip.loadAsync(bytes);
    checkCancellation(signal);
    const entries = Object.values(zip.files);
    if (entries.length > 20000) throw new Error('Document has too many parts');
    let total = 0;
    for (const entry of entries) {
        const size = entry._data?.uncompressedSize ?? 0;
        if (size > 64 * 1024 * 1024) throw new Error('Document part exceeds preview budget');
        total += size;
    }
    if (total > 512 * 1024 * 1024) throw new Error('Expanded document exceeds preview budget');
    const relationships = zip.file('_rels/.rels');
    if (!relationships) throw new Error('Package relationships are missing');
    const path = documentPath(await relationships.async('string'));
    const part = zip.file(path);
    if (!part) throw new Error('Main document part is missing');
    const projected = projectDocumentBody(await part.async('string'));
    checkCancellation(signal);
    if (!projected.truncated) return { bytes, truncated: false };
    zip.file(path, projected.xml);
    const firstBytes = await zip.generateAsync({ type: 'arraybuffer', compression: 'STORE' });
    checkCancellation(signal);
    return { bytes: firstBytes, truncated: true };
}

function capturePosition(container) {
    const paragraphs = [...container.querySelectorAll('p')];
    const index = paragraphs.findIndex(item => item.getBoundingClientRect().bottom > 0);
    const selection = window.getSelection();
    let selected;
    if (selection?.rangeCount && !selection.isCollapsed &&
        container.contains(selection.anchorNode) && container.contains(selection.focusNode)) {
        const offset = (node, index) => {
            const element = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
            const paragraph = element.closest('p');
            const paragraphIndex = paragraphs.indexOf(paragraph);
            const range = document.createRange();
            range.selectNodeContents(paragraphIndex >= 0 ? paragraph : container);
            range.setEnd(node, index);
            return { paragraph: paragraphIndex, offset: range.toString().length };
        };
        const range = selection.getRangeAt(0);
        selected = {
            anchor: offset(selection.anchorNode, selection.anchorOffset),
            focus: offset(selection.focusNode, selection.focusOffset),
            backward: range.startContainer !== selection.anchorNode || range.startOffset !== selection.anchorOffset,
        };
    }
    return { index, top: index < 0 ? 0 : paragraphs[index].getBoundingClientRect().top,
        x: window.scrollX, y: window.scrollY, selected };
}

function restorePosition(container, position) {
    if (position.selected) {
        const paragraphs = container.querySelectorAll('p');
        const point = (endpoint, preferNext) => {
            const root = endpoint.paragraph < 0 ? container : paragraphs[endpoint.paragraph];
            if (!root) return null;
            const nodes = [];
            const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
            while (walker.nextNode()) nodes.push(walker.currentNode);
            let offset = endpoint.offset;
            for (let index = 0; index < nodes.length; ++index) {
                const node = nodes[index];
                if (offset < node.length || (offset === node.length && (!preferNext || index === nodes.length - 1))) return [node, offset];
                offset -= node.length;
            }
            return null;
        };
        const anchor = point(position.selected.anchor, !position.selected.backward);
        const focus = point(position.selected.focus, position.selected.backward);
        if (anchor && focus) window.getSelection().setBaseAndExtent(...anchor, ...focus);
    }
    const paragraph = container.querySelectorAll('p')[position.index];
    window.scrollTo(position.x, paragraph
        ? window.scrollY + paragraph.getBoundingClientRect().top - position.top : position.y);
}

async function visibleAssets(container, signal) {
    container.getBoundingClientRect();
    await document.fonts.ready;
    await Promise.all([...container.querySelectorAll('img')].filter(image => {
        const bounds = image.getBoundingClientRect();
        return bounds.bottom > 0 && bounds.top < window.innerHeight;
    }).map(image => image.decode()));
    checkCancellation(signal);
}

// The owner supplies image conversion and aborts when its preview generation ends.
export async function renderDocumentPreview(bytes, container, { signal, convertImage, onFirstReady } = {}) {
    checkCancellation(signal);
    const allUrls = new Set();
    const dispose = () => {
        signal?.removeEventListener('abort', dispose);
        for (const url of allUrls) URL.revokeObjectURL(url);
        allUrls.clear();
    };
    signal?.addEventListener('abort', dispose, { once: true });
    let activeConversions = 0;
    const waitingConversions = [];
    const convert = async (data, extension) => {
        if (activeConversions < 2) ++activeConversions;
        else await new Promise(resolve => waitingConversions.push(resolve));
        try {
            checkCancellation(signal);
            if (!convertImage) throw new Error('Native image conversion is unavailable');
            const result = await convertImage(data, extension, signal);
            checkCancellation(signal);
            return result;
        } finally {
            const next = waitingConversions.shift();
            if (next) next();
            else --activeConversions;
        }
    };
    const render = async data => {
        const parsed = await parseAsync(data, renderOptions);
        checkCancellation(signal);
        const phaseUrls = new Set();
        const blobToURL = parsed.blobToURL.bind(parsed);
        parsed.blobToURL = (blob, path) => {
            checkCancellation(signal);
            const url = blobToURL(blob, path);
            if (typeof url === 'string' && url.startsWith('blob:')) {
                phaseUrls.add(url);
                allUrls.add(url);
            }
            return url;
        };
        const nativeImage = async (path, fallback) => {
            const extension = path?.split('.').pop().toLowerCase();
            if (!nativeImageExtensions.has(extension)) return fallback();
            const source = await parsed._package.load(path, 'uint8array');
            if (!source || source.byteLength > 8 * 1024 * 1024) throw new Error('Image exceeds preview budget');
            const png = await convert(source, extension);
            const url = URL.createObjectURL(new Blob([png], { type: 'image/png' }));
            phaseUrls.add(url);
            allUrls.add(url);
            return url;
        };
        const loadImage = parsed.loadDocumentImage.bind(parsed);
        parsed.loadDocumentImage = (id, part) => nativeImage(
            parsed.getPathById(part ?? parsed.documentPart, id), () => loadImage(id, part));
        const loadNumberingImage = parsed.loadNumberingImage.bind(parsed);
        parsed.loadNumberingImage = id => nativeImage(
            parsed.getPathById(parsed.numberingPart, id), () => loadNumberingImage(id));
        const nodes = await renderDocument(parsed, renderOptions);
        checkCancellation(signal);
        return { nodes, urls: phaseUrls };
    };
    try {
        let first = await firstDocument(bytes, signal);
        const truncated = first.truncated;
        const initial = await render(first.bytes);
        first = null;
        checkCancellation(signal);
        container.replaceChildren(...initial.nodes);
        await visibleAssets(container, signal);
        onFirstReady?.();
        if (truncated) {
            // Yield until the host has shown the initially hidden native surface.
            await afterFirstPaint(signal);
            checkCancellation(signal);
            const complete = await render(bytes);
            const position = capturePosition(container);
            container.replaceChildren(...complete.nodes);
            await visibleAssets(container, signal);
            restorePosition(container, position);
            for (const url of initial.urls) {
                URL.revokeObjectURL(url);
                allUrls.delete(url);
            }
        }
        return { truncated, dispose };
    } catch (error) {
        dispose();
        signal?.removeEventListener('abort', dispose);
        throw error;
    }
}
